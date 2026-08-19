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

#ifndef MTPUSB_HPP
#define MTPUSB_HPP

#include <cstddef>
#include <mutex>
#include <switch.h>

namespace MTP {
    // usb:ds transport for the MTP responder: brings up a USB Still Image
    // (PTP) interface with the bulk IN / bulk OUT / interrupt IN endpoint trio
    // the class demands, and moves whole blocks over the two bulk pipes.
    //
    // Only one process on the console may own usb:ds at a time, so initialize()
    // fails (rather than waits) when a sysmodule already holds it - the server
    // loop retries later instead of fighting for it.
    class UsbInterface {
    public:
        // Transfer granularity. Every bulk post is at most this many bytes, and
        // both buffers are page-aligned because usbDsEndpoint_PostBufferAsync
        // requires 0x1000 alignment on the address it is handed.
        static constexpr size_t TRANSFER_BUFFER_SIZE = 1024 * 1024;

        // Sentinels returned by read()/write() next to a non-negative byte count.
        static constexpr ssize_t TRANSFER_TIMEOUT = -1;
        static constexpr ssize_t TRANSFER_ERROR   = -2;

        UsbInterface(void) = default;
        ~UsbInterface(void);

        UsbInterface(const UsbInterface&)            = delete;
        UsbInterface& operator=(const UsbInterface&) = delete;

        // Claims usb:ds and publishes the descriptors. False if usb:ds is
        // unavailable, the firmware is too old, or the buffers can't be
        // allocated; nothing is left claimed in that case.
        bool initialize(void);
        void finalize(void);

        bool initialized(void) const { return mInitialized; }

        // True once the host has enumerated the interface and selected a
        // configuration - i.e. a PC is actually on the other end of the cable.
        bool configured(void);
        // Blocks on the usb:ds state-change event until configured() or the
        // timeout expires. False on timeout, so the caller can re-check whether
        // the user has since switched MTP back off.
        bool waitConfigured(u64 timeoutNs);

        // The page-aligned staging buffers. Callers read a received block out of
        // readBuffer() and fill writeBuffer() before calling write(), so file
        // data can be streamed straight into and out of the DMA buffers.
        u8* readBuffer(void) { return mReadBuffer; }
        u8* writeBuffer(void) { return mWriteBuffer; }
        static constexpr size_t bufferSize(void) { return TRANSFER_BUFFER_SIZE; }

        // Bulk OUT: receives at most `size` bytes into readBuffer(). Returns the
        // byte count, TRANSFER_TIMEOUT, or TRANSFER_ERROR.
        ssize_t read(size_t size, u64 timeoutNs);
        // Bulk IN: sends writeBuffer()[0, size). A zero size sends the
        // zero-length packet that terminates a data phase.
        ssize_t write(size_t size, u64 timeoutNs);

        // Aborts any in-flight transfer on both bulk pipes, so a blocked
        // read/write returns instead of pinning the worker thread at shutdown.
        // Safe to call from another thread while the worker is transferring.
        void cancelTransfers(void);

    private:
        bool setupDescriptors(void);
        ssize_t transfer(UsbDsEndpoint* endpoint, void* buffer, size_t size, u64 timeoutNs);

        UsbDsInterface* mInterface   = nullptr;
        UsbDsEndpoint* mEndpointIn   = nullptr;
        UsbDsEndpoint* mEndpointOut  = nullptr;
        UsbDsEndpoint* mEndpointIntr = nullptr;
        // usb:ds calls on one interface share a service session, which is not
        // safe to drive from two threads at once. Every usbDs* call below is made
        // under this; the completion-event wait deliberately is not, so a cancel
        // from another thread can still break a parked transfer.
        std::mutex mUsbMutex;
        u8* mReadBuffer   = nullptr;
        u8* mWriteBuffer  = nullptr;
        bool mInitialized = false;
    };
}

#endif
