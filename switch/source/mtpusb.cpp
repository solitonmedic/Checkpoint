/*
 *   This file is part of Checkpoint
 *   Copyright (C) 2017-2026 Bernardo Giordano, FlagBrew
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
 *       * Requiring preservation of specified reasonable legal notices or
 *         author attributions in that material or in the Appropriate Legal
 *         Notices displayed by works containing it.
 *       * Prohibiting misrepresentation of the origin of that material,
 *         or requiring that modified versions of such material be marked in
 *         reasonable ways as different from the original version.
 */

#include "mtpusb.hpp"
#include "logging.hpp"
#include <cstdint>
#include <cstdlib>
#include <malloc.h>

namespace {
    // usb:ds hands DMA the buffer we post, so the address has to sit on a page
    // boundary. memalign gives us that; plain new/malloc does not guarantee it.
    constexpr size_t USB_BUFFER_ALIGNMENT = 0x1000;

    // USB Still Image class, PIMA 15740 subclass, bulk-only protocol. This trio
    // is what makes a host load its PTP/MTP driver instead of treating us as a
    // vendor-specific device.
    constexpr u8 USB_CLASS_IMAGE          = 0x06;
    constexpr u8 USB_SUBCLASS_STILL_IMAGE = 0x01;
    constexpr u8 USB_PROTOCOL_BULK_ONLY   = 0x01;

    constexpr u16 MAX_PACKET_SIZE_HIGH  = 0x200;
    constexpr u16 MAX_PACKET_SIZE_SUPER = 0x400;
    // The interrupt pipe only ever carries PTP event containers, which are at
    // most a header plus three parameters.
    constexpr u16 MAX_PACKET_SIZE_INTERRUPT = 0x18;

    // Nintendo's own vendor id with the USB-comms product id, the pair every
    // libnx usb:ds device presents. Hosts key their PTP driver off the interface
    // class, not off this, so it needs no product of its own.
    constexpr u16 USB_VENDOR_ID  = 0x057E;
    constexpr u16 USB_PRODUCT_ID = 0x3000;

    // Deliberately not the console's real serial number: MTP hands the serial to
    // every host that enumerates us, and Checkpoint has no reason to publish it.
    constexpr const char* USB_SERIAL_NUMBER = "Checkpoint";

    // USB 2.0 binary object store advertising the USB 2.0 extension and a
    // SuperSpeed device capability, so a USB 3 port negotiates SS instead of
    // falling back. Byte-for-byte the descriptor libnx's own usb_comms publishes.
    const u8 BINARY_OBJECT_STORE[0x16] = {
        // BOS descriptor: 5 bytes, type 0x0F, total length 0x16, 2 capabilities.
        0x05, 0x0F, 0x16, 0x00, 0x02,
        // USB 2.0 extension: LPM supported.
        0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
        // SuperSpeed device capability: full/high/super speeds supported.
        0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};
}

namespace MTP {
    UsbInterface::~UsbInterface(void)
    {
        finalize();
    }

    bool UsbInterface::initialize(void)
    {
        if (mInitialized) {
            return true;
        }

        // The pre-5.0.0 usb:ds interface takes descriptors a different way and
        // is long obsolete on any console that can run Checkpoint; refuse
        // loudly rather than silently registering a broken interface.
        if (hosversionBefore(5, 0, 0)) {
            Logging::error("MTP needs firmware 5.0.0 or newer for the usb:ds interface.");
            return false;
        }

        mReadBuffer  = (u8*)memalign(USB_BUFFER_ALIGNMENT, TRANSFER_BUFFER_SIZE);
        mWriteBuffer = (u8*)memalign(USB_BUFFER_ALIGNMENT, TRANSFER_BUFFER_SIZE);
        if (mReadBuffer == nullptr || mWriteBuffer == nullptr) {
            Logging::error("Failed to allocate the {} byte MTP transfer buffers.", TRANSFER_BUFFER_SIZE);
            finalize();
            return false;
        }

        Result rc = usbDsInitialize();
        if (R_FAILED(rc)) {
            // Almost always "someone else owns usb:ds" (a sysmodule, or the
            // console is in a USB mode of its own). Not fatal: the caller backs
            // off and tries again.
            Logging::warning("usbDsInitialize failed with result 0x{:08X}; MTP cannot start.", rc);
            finalize();
            return false;
        }
        mInitialized = true;

        if (!setupDescriptors()) {
            finalize();
            return false;
        }

        rc = usbDsEnable();
        if (R_FAILED(rc)) {
            Logging::error("usbDsEnable failed with result 0x{:08X}.", rc);
            finalize();
            return false;
        }

        Logging::info("MTP USB interface enabled.");
        return true;
    }

    bool UsbInterface::setupDescriptors(void)
    {
        u8 iManufacturer = 0, iProduct = 0, iSerialNumber = 0, iInterface = 0;
        const u16 languageIds[1] = {0x0409}; // en-US

        Result rc = usbDsAddUsbLanguageStringDescriptor(nullptr, languageIds, 1);
        if (R_SUCCEEDED(rc)) {
            rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo");
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsAddUsbStringDescriptor(&iProduct, "Nintendo Switch");
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsAddUsbStringDescriptor(&iSerialNumber, USB_SERIAL_NUMBER);
        }
        if (R_FAILED(rc)) {
            Logging::error("Failed to publish the MTP USB string descriptors, result 0x{:08X}.", rc);
            return false;
        }

        UsbDeviceDescriptor deviceDescriptor = {
            .bLength            = USB_DT_DEVICE_SIZE,
            .bDescriptorType    = USB_DT_DEVICE,
            .bcdUSB             = 0x0200,
            .bDeviceClass       = 0x00,
            .bDeviceSubClass    = 0x00,
            .bDeviceProtocol    = 0x00,
            .bMaxPacketSize0    = 0x40,
            .idVendor           = USB_VENDOR_ID,
            .idProduct          = USB_PRODUCT_ID,
            .bcdDevice          = 0x0100,
            .iManufacturer      = iManufacturer,
            .iProduct           = iProduct,
            .iSerialNumber      = iSerialNumber,
            .bNumConfigurations = 0x01,
        };
        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &deviceDescriptor);
        if (R_SUCCEEDED(rc)) {
            // SuperSpeed encodes EP0's packet size as a power of two exponent,
            // so 0x09 means 512 bytes - not 9.
            deviceDescriptor.bcdUSB          = 0x0300;
            deviceDescriptor.bMaxPacketSize0 = 0x09;
            rc                               = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &deviceDescriptor);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsSetBinaryObjectStore(BINARY_OBJECT_STORE, sizeof(BINARY_OBJECT_STORE));
        }
        if (R_FAILED(rc)) {
            Logging::error("Failed to publish the MTP USB device descriptors, result 0x{:08X}.", rc);
            return false;
        }

        rc = usbDsRegisterInterface(&mInterface);
        if (R_FAILED(rc)) {
            Logging::error("usbDsRegisterInterface failed with result 0x{:08X}.", rc);
            return false;
        }

        rc = usbDsAddUsbStringDescriptor(&iInterface, "MTP");
        if (R_FAILED(rc)) {
            Logging::error("Failed to publish the MTP interface string, result 0x{:08X}.", rc);
            return false;
        }

        struct usb_interface_descriptor interfaceDescriptor = {
            .bLength            = USB_DT_INTERFACE_SIZE,
            .bDescriptorType    = USB_DT_INTERFACE,
            .bInterfaceNumber   = (u8)mInterface->interface_index,
            .bAlternateSetting  = 0,
            .bNumEndpoints      = 3,
            .bInterfaceClass    = USB_CLASS_IMAGE,
            .bInterfaceSubClass = USB_SUBCLASS_STILL_IMAGE,
            .bInterfaceProtocol = USB_PROTOCOL_BULK_ONLY,
            .iInterface         = iInterface,
        };

        // Endpoint numbers are relative to the interface index: interface 0 gets
        // bulk 0x81/0x01 and interrupt 0x82.
        struct usb_endpoint_descriptor endpointIn = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = (u8)(USB_ENDPOINT_IN + mInterface->interface_index + 1),
            .bmAttributes     = USB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize   = MAX_PACKET_SIZE_HIGH,
            .bInterval        = 0,
        };
        struct usb_endpoint_descriptor endpointOut = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = (u8)(USB_ENDPOINT_OUT + mInterface->interface_index + 1),
            .bmAttributes     = USB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize   = MAX_PACKET_SIZE_HIGH,
            .bInterval        = 0,
        };
        // The class requires an interrupt IN endpoint for asynchronous events.
        // We never post to it, but leaving it out makes strict hosts (Windows in
        // particular) refuse to bind their MTP driver.
        struct usb_endpoint_descriptor endpointInterrupt = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = (u8)(USB_ENDPOINT_IN + mInterface->interface_index + 2),
            .bmAttributes     = USB_TRANSFER_TYPE_INTERRUPT,
            .wMaxPacketSize   = MAX_PACKET_SIZE_INTERRUPT,
            .bInterval        = 0x06,
        };

        struct usb_ss_endpoint_companion_descriptor bulkCompanion = {
            .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
            .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
            .bMaxBurst         = 0x0F,
            .bmAttributes      = 0x00,
            .wBytesPerInterval = 0x00,
        };
        struct usb_ss_endpoint_companion_descriptor interruptCompanion = {
            .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
            .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
            .bMaxBurst         = 0x00,
            .bmAttributes      = 0x00,
            .wBytesPerInterval = 0x00,
        };

        // High speed configuration.
        rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_High, &interfaceDescriptor, USB_DT_INTERFACE_SIZE);
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_High, &endpointIn, USB_DT_ENDPOINT_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_High, &endpointOut, USB_DT_ENDPOINT_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_High, &endpointInterrupt, USB_DT_ENDPOINT_SIZE);
        }

        // SuperSpeed configuration: same descriptors with the bigger bulk packet
        // size, each endpoint followed by its companion descriptor.
        endpointIn.wMaxPacketSize  = MAX_PACKET_SIZE_SUPER;
        endpointOut.wMaxPacketSize = MAX_PACKET_SIZE_SUPER;
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &interfaceDescriptor, USB_DT_INTERFACE_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &endpointIn, USB_DT_ENDPOINT_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &bulkCompanion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &endpointOut, USB_DT_ENDPOINT_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &bulkCompanion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &endpointInterrupt, USB_DT_ENDPOINT_SIZE);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_AppendConfigurationData(mInterface, UsbDeviceSpeed_Super, &interruptCompanion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
        }
        if (R_FAILED(rc)) {
            Logging::error("Failed to publish the MTP configuration descriptors, result 0x{:08X}.", rc);
            return false;
        }

        rc = usbDsInterface_RegisterEndpoint(mInterface, &mEndpointIn, endpointIn.bEndpointAddress);
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_RegisterEndpoint(mInterface, &mEndpointOut, endpointOut.bEndpointAddress);
        }
        if (R_SUCCEEDED(rc)) {
            rc = usbDsInterface_RegisterEndpoint(mInterface, &mEndpointIntr, endpointInterrupt.bEndpointAddress);
        }
        if (R_FAILED(rc)) {
            Logging::error("Failed to register the MTP endpoints, result 0x{:08X}.", rc);
            return false;
        }

        rc = usbDsInterface_EnableInterface(mInterface);
        if (R_FAILED(rc)) {
            Logging::error("usbDsInterface_EnableInterface failed with result 0x{:08X}.", rc);
            return false;
        }

        return true;
    }

    void UsbInterface::finalize(void)
    {
        {
            std::lock_guard<std::mutex> lock(mUsbMutex);
            if (mInitialized) {
                // Cancel inline rather than through cancelTransfers(), which would
                // deadlock on the lock we are already holding.
                if (mEndpointIn != nullptr) {
                    usbDsEndpoint_Cancel(mEndpointIn);
                }
                if (mEndpointOut != nullptr) {
                    usbDsEndpoint_Cancel(mEndpointOut);
                }
                if (mEndpointIntr != nullptr) {
                    usbDsEndpoint_Cancel(mEndpointIntr);
                }
                usbDsDisable();
                usbDsExit();
                mInitialized = false;
            }

            // usbDsExit invalidates the interface/endpoint handles it owns; drop our
            // copies so a stale pointer can't be posted to on a later restart.
            mInterface    = nullptr;
            mEndpointIn   = nullptr;
            mEndpointOut  = nullptr;
            mEndpointIntr = nullptr;
        }

        free(mReadBuffer);
        free(mWriteBuffer);
        mReadBuffer  = nullptr;
        mWriteBuffer = nullptr;
    }

    bool UsbInterface::configured(void)
    {
        if (!mInitialized) {
            return false;
        }
        UsbState state = UsbState_Detached;
        if (R_FAILED(usbDsGetState(&state))) {
            return false;
        }
        return state == UsbState_Configured;
    }

    bool UsbInterface::waitConfigured(u64 timeoutNs)
    {
        if (!mInitialized) {
            return false;
        }
        if (configured()) {
            return true;
        }

        Event* stateChange = usbDsGetStateChangeEvent();
        if (stateChange == nullptr) {
            return false;
        }
        if (R_FAILED(eventWait(stateChange, timeoutNs))) {
            return false;
        }
        eventClear(stateChange);
        return configured();
    }

    ssize_t UsbInterface::transfer(UsbDsEndpoint* endpoint, void* buffer, size_t size, u64 timeoutNs)
    {
        if (!mInitialized || endpoint == nullptr) {
            return TRANSFER_ERROR;
        }

        u32 urbId = 0;
        Result rc;
        {
            std::lock_guard<std::mutex> lock(mUsbMutex);
            rc = usbDsEndpoint_PostBufferAsync(endpoint, buffer, size, &urbId);
        }
        if (R_FAILED(rc)) {
            Logging::debug("usbDsEndpoint_PostBufferAsync failed with result 0x{:08X}.", rc);
            return TRANSFER_ERROR;
        }

        // Deliberately outside the lock: this is where the thread parks, and
        // cancelTransfers() has to be able to take the lock to break it out.
        rc = eventWait(&endpoint->CompletionEvent, timeoutNs);
        if (R_FAILED(rc)) {
            // The URB is still queued and still owns our buffer, so it has to be
            // cancelled and its completion drained before the buffer is reused.
            {
                std::lock_guard<std::mutex> lock(mUsbMutex);
                usbDsEndpoint_Cancel(endpoint);
            }
            eventWait(&endpoint->CompletionEvent, UINT64_MAX);
            eventClear(&endpoint->CompletionEvent);
            return TRANSFER_TIMEOUT;
        }
        eventClear(&endpoint->CompletionEvent);

        UsbDsReportData report;
        {
            std::lock_guard<std::mutex> lock(mUsbMutex);
            rc = usbDsEndpoint_GetReportData(endpoint, &report);
        }
        if (R_FAILED(rc)) {
            return TRANSFER_ERROR;
        }

        u32 transferred = 0;
        rc              = usbDsParseReportData(&report, urbId, nullptr, &transferred);
        if (R_FAILED(rc)) {
            return TRANSFER_ERROR;
        }
        return (ssize_t)transferred;
    }

    ssize_t UsbInterface::read(size_t size, u64 timeoutNs)
    {
        if (size > TRANSFER_BUFFER_SIZE) {
            size = TRANSFER_BUFFER_SIZE;
        }
        return transfer(mEndpointOut, mReadBuffer, size, timeoutNs);
    }

    ssize_t UsbInterface::write(size_t size, u64 timeoutNs)
    {
        if (size > TRANSFER_BUFFER_SIZE) {
            return TRANSFER_ERROR;
        }
        return transfer(mEndpointIn, mWriteBuffer, size, timeoutNs);
    }

    void UsbInterface::cancelTransfers(void)
    {
        std::lock_guard<std::mutex> lock(mUsbMutex);
        if (!mInitialized) {
            return;
        }
        if (mEndpointIn != nullptr) {
            usbDsEndpoint_Cancel(mEndpointIn);
        }
        if (mEndpointOut != nullptr) {
            usbDsEndpoint_Cancel(mEndpointOut);
        }
        if (mEndpointIntr != nullptr) {
            usbDsEndpoint_Cancel(mEndpointIntr);
        }
    }
}
