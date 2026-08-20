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

#include "mtpserver.hpp"
#include "configuration.hpp"
#include "logging.hpp"
#include "mtpresponder.hpp"
#include "mtpusb.hpp"
#include <atomic>
#include <memory>
#include <switch.h>

namespace {
    // The whole SD card, same view the FTP server serves. A save manager's
    // backups are only part of what people came to copy off the card.
    constexpr const char* STORAGE_ROOT        = "sdmc:/";
    constexpr const char* STORAGE_DESCRIPTION = "SD Card";

    // How long a bulk read waits for the host before the loop re-checks whether
    // MTP is still switched on. Short enough that the toggle feels immediate,
    // long enough not to spin.
    constexpr u64 COMMAND_POLL_NS = 500'000'000ULL;
    // Same idea for "plugged in yet?".
    constexpr u64 CONNECT_POLL_NS = 500'000'000ULL;
    // Retry delay after usb:ds refuses to initialize. Long, because the usual
    // cause (another process owns it, or the console is docked) doesn't clear
    // in a hurry and each attempt logs.
    constexpr u64 CLAIM_RETRY_NS = 5'000'000'000ULL;

    std::atomic<bool> serverRunning{false};
    std::atomic<bool> usbClaimed{false};
    std::atomic<bool> hostConnected{false};

    // Owned here rather than by the loop so exit() can cancel an in-flight bulk
    // transfer: without that, closing the app during a copy would block on the
    // join until the data-phase timeout expired.
    std::unique_ptr<MTP::UsbInterface> usb;

    Thread serverThread;
    bool threadValid = false;

    // svcSleepThread in slices, so a shutdown request doesn't have to wait out
    // the whole back-off.
    void interruptibleSleep(u64 nanoseconds)
    {
        constexpr u64 SLICE_NS = 100'000'000ULL;
        while (nanoseconds > 0 && serverRunning.load()) {
            const u64 slice = nanoseconds < SLICE_NS ? nanoseconds : SLICE_NS;
            svcSleepThread(slice);
            nanoseconds -= slice;
        }
    }

    void mtpLoop(void*)
    {
        // Heap-allocated so the object database and its strings never sit on
        // this thread's stack.
        auto responder = std::make_unique<MTP::Responder>(*usb, STORAGE_ROOT, STORAGE_DESCRIPTION);

        while (serverRunning.load()) {
            if (!Configuration::getInstance().isMTPEnabled()) {
                if (usb->initialized()) {
                    Logging::info("MTP switched off; releasing the USB interface.");
                    responder->reset();
                    usb->finalize();
                    usbClaimed.store(false);
                    hostConnected.store(false);
                }
                interruptibleSleep(250'000'000ULL);
                continue;
            }

            if (!usb->initialized()) {
                if (!usb->initialize()) {
                    interruptibleSleep(CLAIM_RETRY_NS);
                    continue;
                }
                usbClaimed.store(true);
            }

            if (!usb->configured()) {
                if (hostConnected.exchange(false)) {
                    // The cable was pulled (or the host suspended us): every
                    // handle the old host held is meaningless now.
                    Logging::info("MTP host disconnected.");
                    responder->reset();
                }
                usb->waitConfigured(CONNECT_POLL_NS);
                continue;
            }

            if (!hostConnected.exchange(true)) {
                Logging::info("MTP host connected.");
            }

            if (!responder->serve(COMMAND_POLL_NS)) {
                // The exchange failed badly enough that the pipe is out of step.
                // Dropping the session is enough: the host reopens one, and the
                // USB interface itself is still valid.
                Logging::warning("Resetting the MTP session after a failed exchange.");
                responder->reset();
            }
        }

        responder->reset();
        usb->finalize();
        usbClaimed.store(false);
        hostConnected.store(false);
    }
}

void MTPServer::init(void)
{
    if (threadValid) {
        return;
    }

    usb = std::make_unique<MTP::UsbInterface>();
    serverRunning.store(true);
    // Same priority and core hint as the FTP and copy workers: this thread is
    // IO-bound and must not compete with the UI thread for the main core.
    if (R_SUCCEEDED(threadCreate(&serverThread, mtpLoop, nullptr, nullptr, 0x10000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&serverThread))) {
        threadValid = true;
        Logging::info("MTP worker started.");
    }
    else {
        serverRunning.store(false);
        usb.reset();
        Logging::error("Failed to start the MTP worker thread.");
    }
}

void MTPServer::exit(void)
{
    if (!threadValid) {
        return;
    }

    serverRunning.store(false);
    // Break any bulk transfer the worker is parked on so the join returns
    // promptly instead of waiting out the data-phase timeout.
    if (usb) {
        usb->cancelTransfers();
    }
    threadWaitForExit(&serverThread);
    threadClose(&serverThread);
    threadValid = false;
    usb.reset();
    Logging::trace("MTP worker stopped");
}

bool MTPServer::isRunning(void)
{
    return usbClaimed.load();
}

bool MTPServer::isConnected(void)
{
    return hostConnected.load();
}
