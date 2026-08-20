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

#ifndef MTPRESPONDER_HPP
#define MTPRESPONDER_HPP

#include "mtp.hpp"
#include "mtpusb.hpp"
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace MTP {
    // The MTP responder proper: one command/data/response exchange at a time,
    // over the bulk pipes of a MTP::UsbInterface, backed by a directory tree on
    // the SD card.
    //
    // Object handles are minted lazily. A host learns about a folder's contents
    // only by asking for them, so a handle is allocated the first time an entry
    // is enumerated (or created) and then kept for the life of the session -
    // hosts cache handles and expect them to stay valid. Handles are never
    // reused within a session, so a stale handle for a deleted file resolves to
    // "no such object" instead of silently pointing at a different file.
    class Responder {
    public:
        // `storageRoot` is the fsdev path the storage maps to (e.g. "sdmc:/");
        // `storageDescription` is the volume name the host displays.
        Responder(UsbInterface& usb, std::string storageRoot, std::string storageDescription);

        // Runs one exchange. Returns false when the transport failed and the
        // caller should tear the USB session down and start over; true when a
        // command was served or nothing arrived before the poll timeout.
        bool serve(u64 pollTimeoutNs);

        // Forgets the open session and every minted handle. Called when the
        // cable is pulled or the feature is switched off, so a reconnecting host
        // starts from a clean database.
        void reset(void);

        bool sessionOpen(void) const { return mSessionOpen; }

    private:
        struct Object {
            std::string path; // full fsdev path, e.g. "sdmc:/switch/Checkpoint"
            u32 parent;       // HANDLE_ROOT for a top-level entry
            bool directory;
        };

        // --- transport helpers ------------------------------------------------
        bool sendResponse(u32 transactionId, u16 responseCode, const std::vector<u32>& parameters = {});
        bool sendData(u32 transactionId, u16 operationCode, const std::vector<uint8_t>& payload);
        // Streams `count` bytes of an already-open file starting at `offset` as
        // the data phase. The caller opens the file so a failure to do so can be
        // answered with a plain error response, before any data block goes out.
        // `sentOut` receives what was actually read off disk.
        bool sendFileData(u32 transactionId, u16 operationCode, FILE* file, u64 offset, u64 count, u64& sentOut);
        // Reads the host's data phase into `payload`. Rejects a block that isn't
        // a data container for this transaction.
        bool receiveData(u32 transactionId, std::vector<uint8_t>& payload);
        // Reads the host's data phase straight to disk. `declaredSize` is the
        // size the matching SendObjectInfo announced, used when the container
        // length is the 0xFFFFFFFF "unknown" marker.
        bool receiveFileData(u32 transactionId, const std::string& path, u64 declaredSize, u16& responseOut);

        // --- object database --------------------------------------------------
        u32 handleFor(const std::string& path, u32 parent, bool directory);
        const Object* object(u32 handle) const;
        // Drops `handle` and everything minted underneath it.
        void forget(u32 handle);
        std::string pathOf(u32 handle) const;
        // Rewrites `handle`'s path (and every descendant's path prefix) after a
        // rename or move, so handles the host already cached stay valid.
        void reparent(u32 handle, const std::string& newPath, u32 newParent);
        // Enumerates `parent` (HANDLE_ROOT = the storage root), minting handles
        // for entries seen for the first time. Returns false if the directory
        // could not be listed.
        bool childrenOf(u32 parent, std::vector<u32>& out);

        // --- operation handlers ----------------------------------------------
        bool onGetDeviceInfo(const Container& command);
        bool onOpenSession(const Container& command);
        bool onCloseSession(const Container& command);
        bool onGetStorageIDs(const Container& command);
        bool onGetStorageInfo(const Container& command);
        bool onGetNumObjects(const Container& command);
        bool onGetObjectHandles(const Container& command);
        bool onGetObjectInfo(const Container& command);
        bool onGetObject(const Container& command);
        bool onGetPartialObject(const Container& command);
        bool onDeleteObject(const Container& command);
        bool onSendObjectInfo(const Container& command);
        bool onSendObject(const Container& command);
        bool onMoveObject(const Container& command);
        bool onGetDevicePropDesc(const Container& command);
        bool onGetDevicePropValue(const Container& command);
        bool onGetObjectPropsSupported(const Container& command);
        bool onGetObjectPropDesc(const Container& command);
        bool onGetObjectPropValue(const Container& command);
        bool onSetObjectPropValue(const Container& command);
        bool onGetObjectPropList(const Container& command);
        bool onGetObjectReferences(const Container& command);

        // Appends one object property value to `builder`; false if the property
        // doesn't apply to this object. `info` is the object's already-taken stat,
        // so building a whole property list costs one stat per object rather than
        // one per property.
        bool appendPropertyValue(DataBuilder& builder, u32 handle, const Object& entry, const struct stat& info, u16 property);
        // Data type of a supported object property, or 0 when unsupported.
        static u16 propertyDataType(u16 property);

        UsbInterface& mUsb;
        std::string mStorageRoot;
        std::string mStorageDescription;

        bool mSessionOpen = false;
        u32 mNextHandle   = 1;
        std::unordered_map<u32, Object> mObjects;
        std::unordered_map<std::string, u32> mHandlesByPath;

        // Set by SendObjectInfo, consumed by the SendObject that must follow it.
        bool mSendPending = false;
        std::string mSendPath;
        u32 mSendHandle = 0;
        u64 mSendSize   = 0;
    };
}

#endif
