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

#ifndef MTPSERVER_HPP
#define MTPSERVER_HPP

// USB Media Transfer Protocol responder: with the console plugged into a PC and
// the MTP toggle on, the SD card shows up as a portable device, so backups can
// be dragged off (and scripts dropped on) without pulling the card or joining a
// network - the cable-based counterpart to the FTP server.
//
// The worker thread owns the whole lifecycle and polls the config toggle, so
// flipping MTP off releases usb:ds and flipping it on claims it again, with no
// restart in between. usb:ds is exclusive to one process, so claiming it can
// fail (a sysmodule holding it, or a docked console in host mode); the loop
// backs off and keeps retrying instead of treating that as fatal.
namespace MTPServer {
    // Spawns the worker thread. Call once, after the SD card is mounted.
    void init(void);

    // Stops and joins the worker, releasing usb:ds. Must run before the app
    // tears down Logging / Configuration, which the loop reads.
    void exit(void);

    // True while the USB interface is claimed and published, i.e. the toggle is
    // on and usb:ds was available.
    bool isRunning(void);

    // True once a host has enumerated and configured the interface: something is
    // on the other end of the cable.
    bool isConnected(void);
}

#endif
