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

#include "mtpresponder.hpp"
#include "directory.hpp"
#include "io.hpp"
#include "logging.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
    // Nanosecond budgets for the two halves of an exchange. A data phase can be
    // a multi-megabyte file chunk on a busy host, so it gets the longer rope;
    // both exist so a host that vanishes mid-transaction can't wedge the worker
    // thread until the app is closed.
    constexpr u64 DATA_TIMEOUT_NS     = 30'000'000'000ULL;
    constexpr u64 RESPONSE_TIMEOUT_NS = 10'000'000'000ULL;

    // Ceiling on a host-sent dataset we buffer whole (an ObjectInfo or a single
    // property value). File payloads never come through that path - they stream
    // straight to disk - so anything larger than this is a malformed request.
    constexpr size_t MAX_DATASET_SIZE = 64 * 1024;

    // The PTP "I don't know the total length" marker for a container header.
    constexpr u32 LENGTH_UNKNOWN = 0xFFFFFFFF;

    void put16(u8* out, u16 value)
    {
        std::memcpy(out, &value, sizeof(value));
    }

    void put32(u8* out, u32 value)
    {
        std::memcpy(out, &value, sizeof(value));
    }

    u32 get32(const u8* in)
    {
        u32 value;
        std::memcpy(&value, in, sizeof(value));
        return value;
    }

    u16 get16(const u8* in)
    {
        u16 value;
        std::memcpy(&value, in, sizeof(value));
        return value;
    }

    std::string joinPath(const std::string& directory, const std::string& name)
    {
        if (directory.empty()) {
            return name;
        }
        if (directory.back() == '/') {
            return directory + name;
        }
        return directory + "/" + name;
    }

    std::string baseName(const std::string& path)
    {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string parentPath(const std::string& path)
    {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            return "";
        }
        // Keep the slash for a root like "sdmc:/" so the result stays a valid
        // directory path instead of collapsing to "sdmc:".
        return slash == 0 || path[slash - 1] == ':' ? path.substr(0, slash + 1) : path.substr(0, slash);
    }

    // A name a host is allowed to create. Path separators and the two dot
    // entries are the traversal vectors: without this check a host could name a
    // file "../../foo" and write outside the storage root.
    bool isSafeName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..") {
            return false;
        }
        return name.find('/') == std::string::npos && name.find('\\') == std::string::npos && name.find(':') == std::string::npos;
    }

    // PTP timestamps are "YYYYMMDDThhmmss" in local time.
    std::string formatTimestamp(time_t when)
    {
        struct tm parts;
        if (localtime_r(&when, &parts) == nullptr) {
            return "";
        }
        char buffer[32];
        if (std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%S", &parts) == 0) {
            return "";
        }
        return buffer;
    }

    // FNV-1a over the object path. MTP's persistent object id has to survive a
    // reconnect, and the path is the only thing about an object that does.
    u64 persistentId(const std::string& path)
    {
        u64 hash = 0xCBF29CE484222325ULL;
        for (unsigned char c : path) {
            hash ^= c;
            hash *= 0x100000001B3ULL;
        }
        return hash;
    }

    const std::vector<u16> SUPPORTED_OBJECT_PROPERTIES = {
        MTP::ObjectProperty_StorageId,
        MTP::ObjectProperty_ObjectFormat,
        MTP::ObjectProperty_ProtectionStatus,
        MTP::ObjectProperty_ObjectSize,
        MTP::ObjectProperty_ObjectFileName,
        MTP::ObjectProperty_DateCreated,
        MTP::ObjectProperty_DateModified,
        MTP::ObjectProperty_ParentObject,
        MTP::ObjectProperty_PersistentUniqueObjectIdentifier,
        MTP::ObjectProperty_Name,
    };

    // Only the two name properties are writable; everything else describes the
    // file rather than naming it, and MTP renames arrive as SetObjectPropValue.
    bool isWritableProperty(u16 property)
    {
        return property == MTP::ObjectProperty_ObjectFileName || property == MTP::ObjectProperty_Name;
    }
}

namespace MTP {
    Responder::Responder(UsbInterface& usb, std::string storageRoot, std::string storageDescription)
        : mUsb(usb), mStorageRoot(std::move(storageRoot)), mStorageDescription(std::move(storageDescription))
    {
    }

    void Responder::reset(void)
    {
        mSessionOpen = false;
        mObjects.clear();
        mHandlesByPath.clear();
        mNextHandle  = 1;
        mSendPending = false;
        mSendPath.clear();
        mSendHandle = 0;
        mSendSize   = 0;
    }

    // ------------------------------------------------------------------ serve

    bool Responder::serve(u64 pollTimeoutNs)
    {
        const ssize_t received = mUsb.read(mUsb.bufferSize(), pollTimeoutNs);
        if (received == UsbInterface::TRANSFER_TIMEOUT) {
            return true; // idle; the caller re-checks whether MTP is still wanted
        }
        if (received == UsbInterface::TRANSFER_ERROR) {
            return false;
        }
        if (received == 0) {
            return true; // stray zero-length packet between transactions
        }
        if ((size_t)received < CONTAINER_HEADER_SIZE) {
            Logging::warning("Discarding a {} byte MTP block: shorter than a container header.", received);
            return true;
        }

        const u8* in = mUsb.readBuffer();
        Container command{};
        command.length        = get32(in);
        command.type          = get16(in + 4);
        command.code          = get16(in + 6);
        command.transactionId = get32(in + 8);

        // Trust the smaller of "what arrived" and "what the header claims", so a
        // lying length can't make us read past the buffer.
        size_t parameterCount = ((size_t)received - CONTAINER_HEADER_SIZE) / sizeof(u32);
        if (command.length >= CONTAINER_HEADER_SIZE) {
            parameterCount = std::min(parameterCount, (size_t)(command.length - CONTAINER_HEADER_SIZE) / sizeof(u32));
        }
        parameterCount = std::min(parameterCount, MAX_PARAMETERS);
        for (size_t i = 0; i < parameterCount; i++) {
            command.parameters[i] = get32(in + CONTAINER_HEADER_SIZE + i * sizeof(u32));
        }
        command.parameterCount = parameterCount;

        if (command.type != ContainerType_Command) {
            // A data or response block with no command in front of it means the
            // pipe is out of step; rebuilding the session is the only way back.
            Logging::warning("Unexpected MTP container type {} (code 0x{:04X}); resetting the session.", command.type, command.code);
            return false;
        }

        // Everything except these two needs a session. Answering anything else
        // without one is what makes a host retry forever instead of reopening.
        if (!mSessionOpen && command.code != Operation_GetDeviceInfo && command.code != Operation_OpenSession) {
            return sendResponse(command.transactionId, Response_SessionNotOpen);
        }

        switch (command.code) {
            case Operation_GetDeviceInfo:
                return onGetDeviceInfo(command);
            case Operation_OpenSession:
                return onOpenSession(command);
            case Operation_CloseSession:
                return onCloseSession(command);
            case Operation_GetStorageIDs:
                return onGetStorageIDs(command);
            case Operation_GetStorageInfo:
                return onGetStorageInfo(command);
            case Operation_GetNumObjects:
                return onGetNumObjects(command);
            case Operation_GetObjectHandles:
                return onGetObjectHandles(command);
            case Operation_GetObjectInfo:
                return onGetObjectInfo(command);
            case Operation_GetObject:
                return onGetObject(command);
            case Operation_GetPartialObject:
                return onGetPartialObject(command);
            case Operation_DeleteObject:
                return onDeleteObject(command);
            case Operation_SendObjectInfo:
                return onSendObjectInfo(command);
            case Operation_SendObject:
                return onSendObject(command);
            case Operation_MoveObject:
                return onMoveObject(command);
            case Operation_GetDevicePropDesc:
                return onGetDevicePropDesc(command);
            case Operation_GetDevicePropValue:
                return onGetDevicePropValue(command);
            case Operation_GetObjectPropsSupported:
                return onGetObjectPropsSupported(command);
            case Operation_GetObjectPropDesc:
                return onGetObjectPropDesc(command);
            case Operation_GetObjectPropValue:
                return onGetObjectPropValue(command);
            case Operation_SetObjectPropValue:
                return onSetObjectPropValue(command);
            case Operation_GetObjectPropList:
                return onGetObjectPropList(command);
            case Operation_GetObjectReferences:
                return onGetObjectReferences(command);
            default:
                Logging::debug("Unsupported MTP operation 0x{:04X}.", command.code);
                return sendResponse(command.transactionId, Response_OperationNotSupported);
        }
    }

    // -------------------------------------------------------------- transport

    bool Responder::sendResponse(u32 transactionId, u16 responseCode, const std::vector<u32>& parameters)
    {
        const size_t count = std::min(parameters.size(), MAX_PARAMETERS);
        const u32 length   = (u32)(CONTAINER_HEADER_SIZE + count * sizeof(u32));
        u8* out            = mUsb.writeBuffer();

        put32(out, length);
        put16(out + 4, ContainerType_Response);
        put16(out + 6, responseCode);
        put32(out + 8, transactionId);
        for (size_t i = 0; i < count; i++) {
            put32(out + CONTAINER_HEADER_SIZE + i * sizeof(u32), parameters[i]);
        }

        return mUsb.write(length, RESPONSE_TIMEOUT_NS) == (ssize_t)length;
    }

    bool Responder::sendData(u32 transactionId, u16 operationCode, const std::vector<uint8_t>& payload)
    {
        const u64 total = CONTAINER_HEADER_SIZE + payload.size();
        u8* out         = mUsb.writeBuffer();

        put32(out, total > LENGTH_UNKNOWN ? LENGTH_UNKNOWN : (u32)total);
        put16(out + 4, ContainerType_Data);
        put16(out + 6, operationCode);
        put32(out + 8, transactionId);

        const size_t first = std::min(payload.size(), mUsb.bufferSize() - CONTAINER_HEADER_SIZE);
        if (first > 0) {
            std::memcpy(out + CONTAINER_HEADER_SIZE, payload.data(), first);
        }
        const size_t firstBlock = CONTAINER_HEADER_SIZE + first;
        if (mUsb.write(firstBlock, DATA_TIMEOUT_NS) != (ssize_t)firstBlock) {
            return false;
        }

        size_t offset = first;
        while (offset < payload.size()) {
            const size_t chunk = std::min(payload.size() - offset, mUsb.bufferSize());
            std::memcpy(out, payload.data() + offset, chunk);
            if (mUsb.write(chunk, DATA_TIMEOUT_NS) != (ssize_t)chunk) {
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool Responder::sendFileData(u32 transactionId, u16 operationCode, FILE* file, u64 offset, u64 count, u64& sentOut)
    {
        sentOut = 0;

        const u64 total = CONTAINER_HEADER_SIZE + count;
        u8* out         = mUsb.writeBuffer();
        put32(out, total > LENGTH_UNKNOWN ? LENGTH_UNKNOWN : (u32)total);
        put16(out + 4, ContainerType_Data);
        put16(out + 6, operationCode);
        put32(out + 8, transactionId);

        if (offset != 0 && fseeko(file, (off_t)offset, SEEK_SET) != 0) {
            return false;
        }

        // First block carries the header, so it can hold that much less payload.
        size_t room      = mUsb.bufferSize() - CONTAINER_HEADER_SIZE;
        size_t headerLen = CONTAINER_HEADER_SIZE;
        u64 remaining    = count;

        while (true) {
            const size_t want = (size_t)std::min<u64>(remaining, room);
            size_t got        = 0;
            if (want > 0) {
                got = fread(out + headerLen, 1, want, file);
                if (got < want) {
                    // The file shrank (or a read failed) after we committed to a
                    // length in the header. Zero-fill so the block still matches
                    // the declared size; the host gets a corrupt tail rather than
                    // a hung pipe, and the short read is logged.
                    std::memset(out + headerLen + got, 0, want - got);
                    Logging::warning("Short read while streaming an MTP object: wanted {}, got {}.", want, got);
                }
            }

            const size_t block = headerLen + want;
            if (mUsb.write(block, DATA_TIMEOUT_NS) != (ssize_t)block) {
                return false;
            }
            sentOut += got;
            remaining -= want;

            if (remaining == 0) {
                break;
            }
            headerLen = 0;
            room      = mUsb.bufferSize();
        }
        return true;
    }

    bool Responder::receiveData(u32 transactionId, std::vector<uint8_t>& payload)
    {
        payload.clear();

        ssize_t received = mUsb.read(mUsb.bufferSize(), DATA_TIMEOUT_NS);
        if (received < (ssize_t)CONTAINER_HEADER_SIZE) {
            return false;
        }

        const u8* in = mUsb.readBuffer();
        if (get16(in + 4) != ContainerType_Data || get32(in + 8) != transactionId) {
            return false;
        }

        const u32 length = get32(in);
        if (length == LENGTH_UNKNOWN || length < CONTAINER_HEADER_SIZE) {
            return false;
        }
        const size_t expected = length - CONTAINER_HEADER_SIZE;
        if (expected > MAX_DATASET_SIZE) {
            Logging::warning("Refusing a {} byte MTP dataset; the cap is {}.", expected, MAX_DATASET_SIZE);
            return false;
        }

        const size_t first = std::min((size_t)received - CONTAINER_HEADER_SIZE, expected);
        payload.assign(in + CONTAINER_HEADER_SIZE, in + CONTAINER_HEADER_SIZE + first);

        while (payload.size() < expected) {
            received = mUsb.read(mUsb.bufferSize(), DATA_TIMEOUT_NS);
            if (received <= 0) {
                return false;
            }
            const size_t take = std::min((size_t)received, expected - payload.size());
            payload.insert(payload.end(), in, in + take);
        }
        return true;
    }

    bool Responder::receiveFileData(u32 transactionId, const std::string& path, u64 declaredSize, u16& responseOut)
    {
        responseOut = Response_GeneralError;

        ssize_t received = mUsb.read(mUsb.bufferSize(), DATA_TIMEOUT_NS);
        if (received < (ssize_t)CONTAINER_HEADER_SIZE) {
            return false;
        }

        const u8* in = mUsb.readBuffer();
        if (get16(in + 4) != ContainerType_Data || get32(in + 8) != transactionId) {
            return false;
        }

        const u32 length = get32(in);
        // A host that streams without knowing the total sends the unknown marker
        // and relies on the size SendObjectInfo already told us.
        const u64 expected = (length == LENGTH_UNKNOWN || length < CONTAINER_HEADER_SIZE) ? declaredSize : length - CONTAINER_HEADER_SIZE;

        FILE* file = fopen(path.c_str(), "wb");
        if (file == nullptr) {
            Logging::error("Failed to create {} for an incoming MTP object, errno {}.", path, errno);
        }

        u64 written  = 0;
        bool ioError = file == nullptr;
        // The first block starts after the container header; every later block is
        // payload from byte zero.
        const u8* src = in + CONTAINER_HEADER_SIZE;
        size_t take   = std::min((size_t)received - CONTAINER_HEADER_SIZE, (size_t)expected);

        while (true) {
            if (!ioError && take > 0 && fwrite(src, 1, take, file) != take) {
                Logging::error("Write of {} failed after {} bytes; the SD card is likely full.", path, written);
                ioError = true;
            }
            written += take;

            if (written >= expected) {
                break;
            }
            // Keep draining the data phase even after a write failure: leaving
            // unread bytes in the pipe desynchronises every later transaction.
            received = mUsb.read(mUsb.bufferSize(), DATA_TIMEOUT_NS);
            if (received <= 0) {
                if (file != nullptr) {
                    fclose(file);
                }
                std::remove(path.c_str());
                return false;
            }
            src  = in;
            take = std::min((size_t)received, (size_t)(expected - written));
        }

        if (file != nullptr) {
            if (fclose(file) != 0) {
                ioError = true;
            }
        }

        if (ioError) {
            std::remove(path.c_str());
            responseOut = Response_StoreFull;
        }
        else {
            responseOut = Response_Ok;
        }
        return true;
    }

    // -------------------------------------------------------- object database

    u32 Responder::handleFor(const std::string& path, u32 parent, bool directory)
    {
        auto existing = mHandlesByPath.find(path);
        if (existing != mHandlesByPath.end()) {
            // Same path, so the same handle - but the entry may have changed kind
            // (a file replaced by a folder) since we last looked at it.
            Object& entry   = mObjects[existing->second];
            entry.parent    = parent;
            entry.directory = directory;
            return existing->second;
        }

        // 0 and 0xFFFFFFFF are the "all objects" / "root" sentinels and must never
        // name a real object. Only reachable after four billion entries in one
        // session, but the wrap is cheaper than the bug it prevents.
        if (mNextHandle == HANDLE_ALL || mNextHandle == HANDLE_ROOT) {
            mNextHandle = 1;
        }
        const u32 handle = mNextHandle++;
        mObjects.emplace(handle, Object{path, parent, directory});
        mHandlesByPath.emplace(path, handle);
        return handle;
    }

    const Responder::Object* Responder::object(u32 handle) const
    {
        auto it = mObjects.find(handle);
        return it == mObjects.end() ? nullptr : &it->second;
    }

    std::string Responder::pathOf(u32 handle) const
    {
        if (handle == HANDLE_ROOT || handle == HANDLE_ALL) {
            return mStorageRoot;
        }
        const Object* entry = object(handle);
        return entry == nullptr ? std::string() : entry->path;
    }

    void Responder::forget(u32 handle)
    {
        const Object* entry = object(handle);
        if (entry == nullptr) {
            return;
        }

        const std::string prefix = entry->path + "/";
        for (auto it = mObjects.begin(); it != mObjects.end();) {
            if (it->first == handle || it->second.path.compare(0, prefix.size(), prefix) == 0) {
                mHandlesByPath.erase(it->second.path);
                it = mObjects.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void Responder::reparent(u32 handle, const std::string& newPath, u32 newParent)
    {
        const Object* entry = object(handle);
        if (entry == nullptr) {
            return;
        }

        const std::string oldPrefix = entry->path + "/";
        const std::string newPrefix = newPath + "/";

        // Collect first: rewriting paths invalidates the by-path index as we go.
        std::vector<std::pair<u32, std::string>> moved;
        for (auto& [objectHandle, object] : mObjects) {
            if (objectHandle == handle) {
                moved.emplace_back(objectHandle, newPath);
            }
            else if (object.path.compare(0, oldPrefix.size(), oldPrefix) == 0) {
                moved.emplace_back(objectHandle, newPrefix + object.path.substr(oldPrefix.size()));
            }
        }

        for (auto& [objectHandle, path] : moved) {
            Object& object = mObjects[objectHandle];
            mHandlesByPath.erase(object.path);
            object.path          = path;
            mHandlesByPath[path] = objectHandle;
        }
        mObjects[handle].parent = newParent;
    }

    bool Responder::childrenOf(u32 parent, std::vector<u32>& out)
    {
        const std::string directory = pathOf(parent);
        if (directory.empty()) {
            return false;
        }

        Directory listing(directory);
        if (!listing.good()) {
            Logging::warning("MTP could not list {} (error 0x{:08X}).", directory, listing.error());
            return false;
        }

        for (size_t i = 0; i < listing.size(); i++) {
            const std::string name = listing.entry(i);
            if (name == "." || name == "..") {
                continue;
            }
            out.push_back(handleFor(joinPath(directory, name), parent, listing.folder(i)));
        }
        return true;
    }

    // ------------------------------------------------------------- operations

    bool Responder::onGetDeviceInfo(const Container& command)
    {
        DataBuilder builder(512);
        builder.addU16(100); // PTP standard version 1.00
        builder.addU32(6);   // vendor extension id: Microsoft MTP
        builder.addU16(100); // vendor extension version 1.00
        builder.addString("microsoft.com: 1.0;");
        builder.addU16(0); // functional mode: standard

        builder.addArrayU16({Operation_GetDeviceInfo, Operation_OpenSession, Operation_CloseSession, Operation_GetStorageIDs,
            Operation_GetStorageInfo, Operation_GetNumObjects, Operation_GetObjectHandles, Operation_GetObjectInfo, Operation_GetObject,
            Operation_DeleteObject, Operation_SendObjectInfo, Operation_SendObject, Operation_GetDevicePropDesc, Operation_GetDevicePropValue,
            Operation_MoveObject, Operation_GetPartialObject, Operation_GetObjectPropsSupported, Operation_GetObjectPropDesc,
            Operation_GetObjectPropValue, Operation_SetObjectPropValue, Operation_GetObjectPropList, Operation_GetObjectReferences});
        // We never push asynchronous events; the interrupt endpoint exists only
        // because the class descriptor requires it.
        builder.addArrayU16({});
        builder.addArrayU16({DeviceProperty_DeviceFriendlyName, DeviceProperty_SynchronizationPartner});
        builder.addArrayU16({}); // capture formats: we are not a camera
        builder.addArrayU16({ObjectFormat_Undefined, ObjectFormat_Association});

        builder.addString("FlagBrew");
        builder.addString("Checkpoint");
        char version[32];
        snprintf(version, sizeof(version), "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO);
        builder.addString(version);
        builder.addString("Checkpoint");

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onOpenSession(const Container& command)
    {
        if (command.parameter(0) == 0) {
            return sendResponse(command.transactionId, Response_InvalidParameter);
        }
        if (mSessionOpen) {
            return sendResponse(command.transactionId, Response_SessionAlreadyOpen);
        }
        // A fresh session gets a fresh handle space: the host is about to
        // enumerate from scratch and must not see handles it never learned.
        reset();
        mSessionOpen = true;
        Logging::info("MTP session opened.");
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onCloseSession(const Container& command)
    {
        reset();
        Logging::info("MTP session closed.");
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetStorageIDs(const Container& command)
    {
        DataBuilder builder(16);
        builder.addArrayU32({STORAGE_ID});
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetStorageInfo(const Container& command)
    {
        if (command.parameter(0) != STORAGE_ID) {
            return sendResponse(command.transactionId, Response_InvalidStorageId);
        }

        // 0xFFFFFFFFFFFFFFFF is MTP's "not reported" for these two. Reporting a
        // guess instead would make a host either refuse a copy that would fit or
        // start one that can't finish.
        u64 capacity  = UINT64_MAX;
        u64 freeSpace = UINT64_MAX;
        struct statvfs stats;
        if (statvfs(mStorageRoot.c_str(), &stats) == 0 && stats.f_frsize != 0) {
            capacity  = (u64)stats.f_blocks * stats.f_frsize;
            freeSpace = (u64)stats.f_bfree * stats.f_frsize;
        }
        else {
            Logging::warning("statvfs on {} failed with errno {}; reporting unknown capacity to the host.", mStorageRoot, errno);
        }

        DataBuilder builder(128);
        builder.addU16(0x0004); // removable RAM
        builder.addU16(0x0002); // generic hierarchical filesystem
        builder.addU16(0x0000); // read-write
        builder.addU64(capacity);
        builder.addU64(freeSpace);
        builder.addU32(0xFFFFFFFF); // free space in objects: not tracked
        builder.addString(mStorageDescription);
        builder.addString(""); // volume identifier

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetNumObjects(const Container& command)
    {
        if (command.parameter(0) != STORAGE_ID && command.parameter(0) != 0xFFFFFFFF) {
            return sendResponse(command.transactionId, Response_InvalidStorageId);
        }

        const u32 parent = command.parameter(2) == HANDLE_ALL ? HANDLE_ROOT : command.parameter(2);
        std::vector<u32> children;
        if (!childrenOf(parent, children)) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }
        return sendResponse(command.transactionId, Response_Ok, {(u32)children.size()});
    }

    bool Responder::onGetObjectHandles(const Container& command)
    {
        if (command.parameter(0) != STORAGE_ID && command.parameter(0) != 0xFFFFFFFF) {
            return sendResponse(command.transactionId, Response_InvalidStorageId);
        }

        const u32 parent = command.parameter(2) == HANDLE_ALL ? HANDLE_ROOT : command.parameter(2);
        std::vector<u32> children;
        if (!childrenOf(parent, children)) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        // The format filter, when set, is only ever "folders" or "plain files"
        // here - those are the only two formats we mint.
        const u32 format = command.parameter(1);
        if (format != 0) {
            std::vector<u32> filtered;
            for (u32 handle : children) {
                const Object* entry = object(handle);
                if (entry != nullptr && (entry->directory ? ObjectFormat_Association : ObjectFormat_Undefined) == format) {
                    filtered.push_back(handle);
                }
            }
            children.swap(filtered);
        }

        DataBuilder builder(8 + children.size() * sizeof(u32));
        builder.addArrayU32(children);
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObjectInfo(const Container& command)
    {
        const Object* entry = object(command.parameter(0));
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        struct stat info;
        if (stat(entry->path.c_str(), &info) != 0) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        const bool directory = S_ISDIR(info.st_mode);
        // ObjectCompressedSize is 32-bit. Anything larger reports the saturated
        // value; hosts that care read ObjectSize (64-bit) off the property list.
        const u64 size          = directory ? 0 : (u64)info.st_size;
        const u32 truncatedSize = size > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (u32)size;

        DataBuilder builder(256);
        builder.addU32(STORAGE_ID);
        builder.addU16(directory ? ObjectFormat_Association : ObjectFormat_Undefined);
        builder.addU16(0); // protection status: none
        builder.addU32(truncatedSize);
        builder.addU16(0); // thumb format
        builder.addU32(0); // thumb compressed size
        builder.addU32(0); // thumb pix width
        builder.addU32(0); // thumb pix height
        builder.addU32(0); // image pix width
        builder.addU32(0); // image pix height
        builder.addU32(0); // image bit depth
        builder.addU32(entry->parent == HANDLE_ROOT ? 0 : entry->parent);
        builder.addU16(directory ? AssociationType_GenericFolder : AssociationType_Undefined);
        builder.addU32(0); // association description
        builder.addU32(0); // sequence number
        builder.addString(baseName(entry->path));
        builder.addString(formatTimestamp(info.st_mtime)); // capture date
        builder.addString(formatTimestamp(info.st_mtime)); // modification date
        builder.addString("");                             // keywords

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObject(const Container& command)
    {
        const Object* entry = object(command.parameter(0));
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        struct stat info;
        if (stat(entry->path.c_str(), &info) != 0) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }
        if (S_ISDIR(info.st_mode)) {
            // A folder has no byte stream; the host should be walking its
            // children instead of asking for its contents.
            return sendResponse(command.transactionId, Response_InvalidObjectFormatCode);
        }

        FILE* file = fopen(entry->path.c_str(), "rb");
        if (file == nullptr) {
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        u64 sent      = 0;
        const bool ok = sendFileData(command.transactionId, command.code, file, 0, (u64)info.st_size, sent);
        fclose(file);
        if (!ok) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetPartialObject(const Container& command)
    {
        const Object* entry = object(command.parameter(0));
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        struct stat info;
        if (stat(entry->path.c_str(), &info) != 0 || S_ISDIR(info.st_mode)) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        const u64 fileSize = (u64)info.st_size;
        const u64 offset   = command.parameter(1);
        const u64 wanted   = command.parameter(2);
        const u64 count    = offset >= fileSize ? 0 : std::min(wanted, fileSize - offset);

        FILE* file = fopen(entry->path.c_str(), "rb");
        if (file == nullptr) {
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        u64 sent      = 0;
        const bool ok = sendFileData(command.transactionId, command.code, file, offset, count, sent);
        fclose(file);
        if (!ok) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok, {(u32)count});
    }

    bool Responder::onDeleteObject(const Container& command)
    {
        const u32 handle = command.parameter(0);
        if (handle == HANDLE_ROOT || handle == HANDLE_ALL) {
            // "Delete every object on the storage" is a legal PTP request and a
            // terrible thing to honour on someone's SD card by accident.
            Logging::warning("Refusing an MTP delete-all request.");
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        const Object* entry = object(handle);
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        const std::string path = entry->path;
        bool removed           = false;
        if (entry->directory) {
            removed = io::deleteFolderRecursively(path) == 0;
        }
        else {
            removed = std::remove(path.c_str()) == 0;
        }

        if (!removed) {
            Logging::warning("MTP failed to delete {} (errno {}).", path, errno);
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        forget(handle);
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onSendObjectInfo(const Container& command)
    {
        const u32 storage = command.parameter(0);
        if (storage != 0 && storage != STORAGE_ID) {
            return sendResponse(command.transactionId, Response_InvalidStorageId);
        }

        std::vector<uint8_t> payload;
        if (!receiveData(command.transactionId, payload)) {
            return false;
        }

        DataParser parser(payload.data(), payload.size());
        parser.skip(sizeof(u32)); // storage id (the parameter wins)
        const u16 format = parser.readU16();
        parser.skip(sizeof(u16)); // protection status
        const u64 size = parser.readU32();
        parser.skip(sizeof(u16));     // thumb format
        parser.skip(sizeof(u32) * 6); // thumb + image dimensions
        parser.skip(sizeof(u32));     // parent object (the parameter wins)
        parser.skip(sizeof(u16));     // association type
        parser.skip(sizeof(u32));     // association description
        parser.skip(sizeof(u32));     // sequence number
        const std::string name = parser.readString();
        if (!parser.ok()) {
            return sendResponse(command.transactionId, Response_InvalidDataset);
        }
        if (!isSafeName(name)) {
            Logging::warning("Rejecting MTP object name \"{}\".", name);
            return sendResponse(command.transactionId, Response_InvalidDataset);
        }

        const u32 parent            = command.parameter(1) == HANDLE_ALL ? HANDLE_ROOT : command.parameter(1);
        const std::string directory = pathOf(parent);
        if (directory.empty() || !io::directoryExists(directory)) {
            return sendResponse(command.transactionId, Response_InvalidParentObject);
        }
        const std::string path = joinPath(directory, name);

        if (format == ObjectFormat_Association) {
            // Folders are created here and now: no SendObject follows one.
            if (!io::directoryExists(path) && R_FAILED(io::createDirectory(path))) {
                Logging::error("MTP failed to create the folder {}.", path);
                return sendResponse(command.transactionId, Response_AccessDenied);
            }
            const u32 handle = handleFor(path, parent, true);
            mSendPending     = false;
            return sendResponse(command.transactionId, Response_Ok, {STORAGE_ID, parent, handle});
        }

        // A file: remember where the SendObject that must follow should land.
        // Nothing is written until its data phase arrives.
        mSendHandle  = handleFor(path, parent, false);
        mSendPath    = path;
        mSendSize    = size;
        mSendPending = true;
        return sendResponse(command.transactionId, Response_Ok, {STORAGE_ID, parent, mSendHandle});
    }

    bool Responder::onSendObject(const Container& command)
    {
        if (!mSendPending) {
            // SendObject is only legal directly after a SendObjectInfo that
            // described a file.
            return sendResponse(command.transactionId, Response_NoValidObjectInfo);
        }

        const std::string path = mSendPath;
        const u32 handle       = mSendHandle;
        const u64 size         = mSendSize;
        mSendPending           = false;

        u16 response = Response_GeneralError;
        if (!receiveFileData(command.transactionId, path, size, response)) {
            // The data phase died mid-flight: the handle we minted points at a
            // file that no longer exists, so retire it.
            forget(handle);
            return false;
        }

        if (response != Response_Ok) {
            forget(handle);
        }
        return sendResponse(command.transactionId, response);
    }

    bool Responder::onMoveObject(const Container& command)
    {
        const Object* entry = object(command.parameter(0));
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }
        const u32 storage = command.parameter(1);
        if (storage != 0 && storage != STORAGE_ID) {
            return sendResponse(command.transactionId, Response_InvalidStorageId);
        }

        const u32 parent            = command.parameter(2) == HANDLE_ALL ? HANDLE_ROOT : command.parameter(2);
        const std::string directory = pathOf(parent);
        if (directory.empty() || !io::directoryExists(directory)) {
            return sendResponse(command.transactionId, Response_InvalidParentObject);
        }

        const u32 handle       = command.parameter(0);
        const std::string from = entry->path;
        const std::string to   = joinPath(directory, baseName(from));
        if (from == to) {
            return sendResponse(command.transactionId, Response_Ok);
        }
        // Moving a folder into itself would detach the subtree from the storage.
        if (to.compare(0, from.size() + 1, from + "/") == 0) {
            return sendResponse(command.transactionId, Response_InvalidParentObject);
        }
        if (rename(from.c_str(), to.c_str()) != 0) {
            Logging::warning("MTP failed to move {} to {} (errno {}).", from, to, errno);
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        reparent(handle, to, parent);
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetDevicePropDesc(const Container& command)
    {
        const u16 property = (u16)command.parameter(0);
        if (property != DeviceProperty_DeviceFriendlyName && property != DeviceProperty_SynchronizationPartner) {
            return sendResponse(command.transactionId, Response_DevicePropNotSupported);
        }

        const std::string value = property == DeviceProperty_DeviceFriendlyName ? "Checkpoint" : "";
        DataBuilder builder(64);
        builder.addU16(property);
        builder.addU16(DataType_String);
        builder.addU8(0);         // read only
        builder.addString(value); // factory default
        builder.addString(value); // current value
        builder.addU8(0);         // no form

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetDevicePropValue(const Container& command)
    {
        const u16 property = (u16)command.parameter(0);
        if (property != DeviceProperty_DeviceFriendlyName && property != DeviceProperty_SynchronizationPartner) {
            return sendResponse(command.transactionId, Response_DevicePropNotSupported);
        }

        DataBuilder builder(32);
        builder.addString(property == DeviceProperty_DeviceFriendlyName ? "Checkpoint" : "");
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObjectReferences(const Container& command)
    {
        if (object(command.parameter(0)) == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }
        // Nothing in a filesystem tree references anything else, but hosts ask
        // and treat a failure as a broken device.
        DataBuilder builder(8);
        builder.addArrayU32({});
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    // ----------------------------------------------------- object properties

    u16 Responder::propertyDataType(u16 property)
    {
        switch (property) {
            case ObjectProperty_StorageId:
            case ObjectProperty_ParentObject:
                return DataType_UInt32;
            case ObjectProperty_ObjectFormat:
            case ObjectProperty_ProtectionStatus:
                return DataType_UInt16;
            case ObjectProperty_ObjectSize:
                return DataType_UInt64;
            case ObjectProperty_PersistentUniqueObjectIdentifier:
                return DataType_UInt128;
            case ObjectProperty_ObjectFileName:
            case ObjectProperty_DateCreated:
            case ObjectProperty_DateModified:
            case ObjectProperty_Name:
                return DataType_String;
            default:
                return 0;
        }
    }

    namespace {
        // Writes the zero/empty value of `type`, used as the factory default in
        // a property description.
        void appendDefaultValue(DataBuilder& builder, u16 type)
        {
            switch (type) {
                case DataType_UInt16:
                    builder.addU16(0);
                    break;
                case DataType_UInt32:
                    builder.addU32(0);
                    break;
                case DataType_UInt64:
                    builder.addU64(0);
                    break;
                case DataType_UInt128:
                    builder.addU128(0, 0);
                    break;
                case DataType_String:
                default:
                    builder.addString("");
                    break;
            }
        }
    }

    bool Responder::appendPropertyValue(DataBuilder& builder, u32 handle, const Object& entry, const struct stat& info, u16 property)
    {
        switch (property) {
            case ObjectProperty_StorageId:
                builder.addU32(STORAGE_ID);
                return true;
            case ObjectProperty_ObjectFormat:
                builder.addU16(entry.directory ? ObjectFormat_Association : ObjectFormat_Undefined);
                return true;
            case ObjectProperty_ProtectionStatus:
                builder.addU16(0);
                return true;
            case ObjectProperty_ObjectSize:
                builder.addU64(entry.directory ? 0 : (u64)info.st_size);
                return true;
            case ObjectProperty_ObjectFileName:
            case ObjectProperty_Name:
                builder.addString(baseName(entry.path));
                return true;
            case ObjectProperty_DateCreated:
            case ObjectProperty_DateModified:
                builder.addString(formatTimestamp(info.st_mtime));
                return true;
            case ObjectProperty_ParentObject:
                builder.addU32(entry.parent == HANDLE_ROOT ? 0 : entry.parent);
                return true;
            case ObjectProperty_PersistentUniqueObjectIdentifier:
                // The handle alone would not survive a reconnect, so the low half
                // hashes the path and the high half carries the handle for
                // in-session uniqueness if two paths ever collide.
                builder.addU128(persistentId(entry.path), handle);
                return true;
            default:
                return false;
        }
    }

    bool Responder::onGetObjectPropsSupported(const Container& command)
    {
        const u32 format = command.parameter(0);
        if (format != 0 && format != ObjectFormat_Undefined && format != ObjectFormat_Association) {
            return sendResponse(command.transactionId, Response_InvalidObjectFormatCode);
        }

        DataBuilder builder(64);
        builder.addArrayU16(SUPPORTED_OBJECT_PROPERTIES);
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObjectPropDesc(const Container& command)
    {
        const u16 property = (u16)command.parameter(0);
        const u16 type     = propertyDataType(property);
        if (type == 0) {
            return sendResponse(command.transactionId, Response_ObjectPropNotSupported);
        }

        DataBuilder builder(64);
        builder.addU16(property);
        builder.addU16(type);
        builder.addU8(isWritableProperty(property) ? 1 : 0);
        appendDefaultValue(builder, type);
        builder.addU32(0); // group code
        builder.addU8(0);  // no form

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObjectPropValue(const Container& command)
    {
        const u32 handle    = command.parameter(0);
        const Object* entry = object(handle);
        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        const u16 property = (u16)command.parameter(1);
        if (propertyDataType(property) == 0) {
            return sendResponse(command.transactionId, Response_ObjectPropNotSupported);
        }

        struct stat info;
        if (stat(entry->path.c_str(), &info) != 0) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        DataBuilder builder(64);
        if (!appendPropertyValue(builder, handle, *entry, info, property)) {
            return sendResponse(command.transactionId, Response_ObjectPropNotSupported);
        }
        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onSetObjectPropValue(const Container& command)
    {
        const u32 handle    = command.parameter(0);
        const Object* entry = object(handle);
        const u16 property  = (u16)command.parameter(1);

        // The data phase has to be consumed whatever the outcome, or the pipe is
        // left holding bytes that would be read as the next command.
        std::vector<uint8_t> payload;
        if (!receiveData(command.transactionId, payload)) {
            return false;
        }

        if (entry == nullptr) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }
        if (propertyDataType(property) == 0) {
            return sendResponse(command.transactionId, Response_ObjectPropNotSupported);
        }
        if (!isWritableProperty(property)) {
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        DataParser parser(payload.data(), payload.size());
        const std::string name = parser.readString();
        if (!parser.ok()) {
            return sendResponse(command.transactionId, Response_InvalidObjectPropValue);
        }
        if (!isSafeName(name)) {
            Logging::warning("Rejecting MTP rename to \"{}\".", name);
            return sendResponse(command.transactionId, Response_InvalidObjectPropValue);
        }

        const std::string from = entry->path;
        const std::string to   = joinPath(parentPath(from), name);
        if (from == to) {
            return sendResponse(command.transactionId, Response_Ok);
        }
        if (rename(from.c_str(), to.c_str()) != 0) {
            Logging::warning("MTP failed to rename {} to {} (errno {}).", from, to, errno);
            return sendResponse(command.transactionId, Response_AccessDenied);
        }

        reparent(handle, to, mObjects[handle].parent);
        return sendResponse(command.transactionId, Response_Ok);
    }

    bool Responder::onGetObjectPropList(const Container& command)
    {
        // A group code selects a vendor-defined bundle of properties; we publish
        // none, so anything but "no grouping" has to be refused explicitly.
        if (command.parameter(3) != 0) {
            return sendResponse(command.transactionId, Response_SpecificationByGroupUnsupported);
        }

        const u32 depth = command.parameter(4);
        if (depth > 1) {
            // Walking the whole card for one request would stall the host for
            // minutes on a full SD; hosts fall back to per-folder queries.
            return sendResponse(command.transactionId, Response_SpecificationByDepthUnsupported);
        }

        const u32 handle = command.parameter(0);
        std::vector<u32> targets;
        if (depth == 0) {
            if (handle == HANDLE_ROOT || handle == HANDLE_ALL || object(handle) == nullptr) {
                return sendResponse(command.transactionId, Response_InvalidObjectHandle);
            }
            targets.push_back(handle);
        }
        else if (!childrenOf(handle == HANDLE_ALL ? HANDLE_ROOT : handle, targets)) {
            return sendResponse(command.transactionId, Response_InvalidObjectHandle);
        }

        const u32 format    = command.parameter(1);
        const u32 requested = command.parameter(2);
        // 0 and 0xFFFFFFFF both mean "every supported property" in the wild.
        const bool allProperties = requested == 0 || requested == 0xFFFFFFFF;
        if (!allProperties && propertyDataType((u16)requested) == 0) {
            return sendResponse(command.transactionId, Response_ObjectPropNotSupported);
        }

        DataBuilder elements(1024);
        u32 count = 0;
        for (u32 target : targets) {
            const Object* entry = object(target);
            if (entry == nullptr) {
                continue;
            }
            if (format != 0 && (entry->directory ? ObjectFormat_Association : ObjectFormat_Undefined) != format) {
                continue;
            }

            struct stat info;
            if (stat(entry->path.c_str(), &info) != 0) {
                continue; // vanished between the listing and now
            }

            for (u16 property : SUPPORTED_OBJECT_PROPERTIES) {
                if (!allProperties && property != (u16)requested) {
                    continue;
                }
                DataBuilder value(64);
                if (!appendPropertyValue(value, target, *entry, info, property)) {
                    continue;
                }
                elements.addU32(target);
                elements.addU16(property);
                elements.addU16(propertyDataType(property));
                elements.addRaw(value.data().data(), value.size());
                count++;
            }
        }

        DataBuilder builder(elements.size() + sizeof(u32));
        builder.addU32(count);
        builder.addRaw(elements.data().data(), elements.size());

        if (!sendData(command.transactionId, command.code, builder.data())) {
            return false;
        }
        return sendResponse(command.transactionId, Response_Ok);
    }
}
