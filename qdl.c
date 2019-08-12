/*
 * Copyright (c) 2016-2017, Linaro Ltd.
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <libusb.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "qdl.h"
#include "patch.h"
#include "ufs.h"

#define MAX_USBFS_BULK_SIZE    (16*1024)

enum {
    QDL_FILE_UNKNOWN,
    QDL_FILE_PATCH,
    QDL_FILE_PROGRAM,
    QDL_FILE_UFS,
    QDL_FILE_CONTENTS,
};

struct qdl_device {
    libusb_device_handle *handle;

    int in_ep;
    int out_ep;

    size_t in_maxpktsize;
    size_t out_maxpktsize;
};

bool qdl_debug;

static int detect_type(const char *xml_file) {
    xmlNode *root;
    xmlDoc *doc;
    xmlNode *node;
    int type = QDL_FILE_UNKNOWN;

    doc = xmlReadFile(xml_file, NULL, 0);
    if (!doc) {
        fprintf(stderr, "[PATCH] failed to parse %s\n", xml_file);
        return -EINVAL;
    }

    root = xmlDocGetRootElement(doc);
    if (!xmlStrcmp(root->name, (xmlChar *) "patches")) {
        type = QDL_FILE_PATCH;
    } else if (!xmlStrcmp(root->name, (xmlChar *) "data")) {
        for (node = root->children; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE)
                continue;
            if (!xmlStrcmp(node->name, (xmlChar *) "program")) {
                type = QDL_FILE_PROGRAM;
                break;
            }
            if (!xmlStrcmp(node->name, (xmlChar *) "ufs")) {
                type = QDL_FILE_UFS;
                break;
            }
        }
    } else if (!xmlStrcmp(root->name, (xmlChar *) "contents")) {
        type = QDL_FILE_CONTENTS;
    }

    xmlFreeDoc(doc);

    return type;
}

static int parse_usb_desc(libusb_device *device, struct qdl_device *qdl, int *intf) {
    unsigned out;
    unsigned in;
    size_t out_size;
    size_t in_size;

    struct libusb_device_descriptor desc = {0};
    int ret = libusb_get_device_descriptor(device, &desc);
    if (ret) {
        err(1, "libusb_get_device_descriptor error %d", ret);
    }

#ifdef DEBUG_PARSE
    printf("=================\n");
    printf("DEBUG_PARSE: desc.idVendor: 0x%04x - desc.idProduct: 0x%04x\n",
           desc.idVendor, desc.idProduct);
    printf("DEBUG_PARSE: desc.bDescriptorType: %d\n", desc.bDescriptorType);
    printf("DEBUG_PARSE: desc.bNumConfigurations: %d\n", desc.bNumConfigurations);
#else
    if (desc.idVendor != 0x05c6 || desc.idProduct != 0x9008) {
        return -EINVAL;
    }

    if (desc.bDescriptorType != LIBUSB_DT_DEVICE) {
        return -EINVAL;
    }
#endif

    for (int i = 0; i < desc.bNumConfigurations; i++) {
        struct libusb_config_descriptor *config;
        ret = libusb_get_config_descriptor(device, i, &config);

        if (ret) {
            err(1, "libusb_get_config_descriptor error");
        }

#ifdef DEBUG_PARSE
        printf("DEBUG_PARSE: config->bDescriptorType: %d\n", config->bDescriptorType);
        printf("DEBUG_PARSE: config->bNumInterfaces: %d\n", config->bNumInterfaces);
#else
        if (config->bDescriptorType != LIBUSB_DT_CONFIG) {
            return -EINVAL;
        }
#endif
        for (int j = 0; j < config->bNumInterfaces; j++) {
            struct libusb_interface interface = config->interface[j];
            if (interface.altsetting->bDescriptorType != LIBUSB_DT_INTERFACE) {
                return -EINVAL;
            }
            if (interface.altsetting->bLength < LIBUSB_DT_INTERFACE_SIZE) {
                return -EINVAL;
            }
            for (int k = 0; k < interface.altsetting->bNumEndpoints; k++) {
                struct libusb_endpoint_descriptor endpoint = interface.altsetting->endpoint[k];
                if (endpoint.bDescriptorType != LIBUSB_DT_ENDPOINT) {
                    return -EINVAL;
                }

                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }

                if (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in = endpoint.bEndpointAddress;
                    in_size = endpoint.wMaxPacketSize;
                } else {
                    out = endpoint.bEndpointAddress;
                    out_size = endpoint.wMaxPacketSize;
                }
            }
            if (interface.altsetting->bInterfaceClass != 0xff) {
                continue;
            }

            if (interface.altsetting->bInterfaceSubClass != 0xff) {
                continue;
            }

            if (interface.altsetting->bInterfaceProtocol != 0xff &&
                interface.altsetting->bInterfaceProtocol != 16) {
                continue;
            }

            libusb_device_handle *handle;
            ret = libusb_open(device, &handle);
            if (ret) {
                err(1, "failed to open, errcode: %d", ret);
            }
            qdl->handle = handle;
            qdl->in_ep = in;
            qdl->in_maxpktsize = in_size;
            qdl->out_ep = out;
            qdl->out_maxpktsize = out_size;

            *intf = interface.altsetting->bInterfaceNumber;
            return 0;
        }
    }
    return -EINVAL;
}

static int usb_open(struct qdl_device *qdl) {

    int intf = -1;

    int ret = libusb_init(NULL);
    if (ret) {
        err(1, "failed to initialize libusb %d", ret);
    }
    libusb_device **usb;
    ssize_t usb_size = libusb_get_device_list(NULL, &usb);
    if (usb_size < 0) {
        err(1, "can't get usb devices.\n");
    }
//    printf("found %ld usb devices.\n", usb_size);
    if (usb_size > 0) {
        for (int i = 0; i < usb_size; i++) {
            ret = parse_usb_desc(usb[i], qdl, &intf);
            if (!ret) {
//                printf("found!!!!!\n");
                goto found;
            }
        }
    }

    return -ENOENT;

    found:
    libusb_free_device_list(usb, usb_size);

    ret = libusb_claim_interface(qdl->handle, intf);
    if (ret) {
        err(1, "libusb_claim_interface");
    }
    return 0;
}

int qdl_read(struct qdl_device *qdl, void *buf, size_t len, unsigned int timeout) {

    int transferred,
            ret = libusb_bulk_transfer(qdl->handle, qdl->in_ep, buf, len, &transferred, timeout);
    return ret ? ret : transferred;
}

int qdl_write(struct qdl_device *qdl, const void *buf, size_t len, bool eot) {

    int transferred = 0, writed = 0, ret = -1, size = len;
    unsigned char *data = (unsigned char *) buf;
    while (size > 0) {
        int xfer = (size > qdl->out_maxpktsize) ? qdl->out_maxpktsize : size;
        ret = libusb_bulk_transfer(qdl->handle, qdl->out_ep, data, xfer, &transferred, 0);
        if (ret) {
            err(1, "libusb_bulk_transfer error");
        }
//        printf("libusb_bulk_transfer: writed: %d - xfer: %d - transferred: %d", writed, xfer, transferred);
        writed += xfer;
        size -= xfer;
        data += xfer;
    }

    if (eot && len % qdl->out_maxpktsize == 0) {
        ret = libusb_bulk_transfer(qdl->handle, qdl->out_ep, NULL, 0, &transferred, 0);
        if (ret) {
            err(1, "libusb_bulk_transfer error");
        }
    }

    return writed;
}

#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define RESET "\x1B[0m"

static void print_usage(void) {
    extern const char *__progname;
    fprintf(stderr, "only for "RED"M01"RESET" usage:\n");
    fprintf(stderr,
            MAG"\t%s --debug --storage emmc ./prog_emmc_firehose_8996_ddr.elf rawprogram_unsparse.xml patch0.xml\n\n"RESET,
            __progname);
    fprintf(stderr,
            "%s [--debug] [--storage <emmc|ufs>] [--finalize-provisioning] [--include <PATH>] <prog.mbn> [<program> <patch> ...]\n",
            __progname);
}

int main(int argc, char **argv) {
    char *prog_mbn, *storage = "ufs";
    char *incdir = NULL;
    int type;
    int ret;
    int opt;
    bool qdl_finalize_provisioning = false;
    struct qdl_device qdl;


    static struct option options[] = {
            {"debug",                 no_argument,       0, 'd'},
            {"include",               required_argument, 0, 'i'},
            {"finalize-provisioning", no_argument,       0, 'l'},
            {"storage",               required_argument, 0, 's'},
            {0, 0,                                       0, 0}
    };

    while ((opt = getopt_long(argc, argv, "di:", options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                qdl_debug = true;
                break;
            case 'i':
                incdir = optarg;
                break;
            case 'l':
                qdl_finalize_provisioning = true;
                break;
            case 's':
                storage = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    /* at least 2 non optional args required */
    if ((optind + 2) > argc) {
        print_usage();
        return 1;
    }

    prog_mbn = argv[optind++];

    do {
        type = detect_type(argv[optind]);
        if (type < 0 || type == QDL_FILE_UNKNOWN)
            errx(1, "failed to detect file type of %s\n", argv[optind]);

        switch (type) {
            case QDL_FILE_PATCH:
                ret = patch_load(argv[optind]);
                if (ret < 0)
                    errx(1, "patch_load %s failed", argv[optind]);
                break;
            case QDL_FILE_PROGRAM:
                ret = program_load(argv[optind]);
                if (ret < 0)
                    errx(1, "program_load %s failed", argv[optind]);
                break;
            case QDL_FILE_UFS:
                ret = ufs_load(argv[optind], qdl_finalize_provisioning);
                if (ret < 0)
                    errx(1, "ufs_load %s failed", argv[optind]);
                break;
            default:
                errx(1, "%s type not yet supported", argv[optind]);
                break;
        }
    } while (++optind < argc);

    ret = usb_open(&qdl);
    if (ret)
        return 1;

    ret = sahara_run(&qdl, prog_mbn);
    if (ret < 0)
        return 1;

    ret = firehose_run(&qdl, incdir, storage);
    if (ret < 0)
        return 1;

    return 0;
}
