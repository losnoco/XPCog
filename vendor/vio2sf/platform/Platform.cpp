/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <vio2sf/Platform.h>
#include <vio2sf/SPI_Firmware.h>

#ifdef __WIN32__
#include <io.h>
#define fdopen _fdopen
#define fseek _fseeki64
#define ftell _ftelli64
#define dup _dup
#endif // __WIN32__

namespace melonDS::Platform
{

void SignalStop(StopReason reason, void* userdata)
{
}


constexpr char AccessMode(FileMode mode, bool file_exists)
{
    if (mode & FileMode::Append)
        return  'a';

    if (!(mode & FileMode::Write))
        // If we're only opening the file for reading...
        return 'r';

    if (mode & (FileMode::NoCreate))
        // If we're not allowed to create a new file...
        return 'r'; // Open in "r+" mode (IsExtended will add the "+")

    if ((mode & FileMode::Preserve) && file_exists)
        // If we're not allowed to overwrite a file that already exists...
        return 'r'; // Open in "r+" mode (IsExtended will add the "+")

    return 'w';
}

constexpr bool IsExtended(FileMode mode)
{
    // fopen's "+" flag always opens the file for read/write
    return (mode & FileMode::ReadWrite) == FileMode::ReadWrite;
}

static std::string GetModeString(FileMode mode, bool file_exists)
{
    std::string modeString;

    modeString += AccessMode(mode, file_exists);

    if (IsExtended(mode))
        modeString += '+';

    if (!(mode & FileMode::Text))
        modeString += 'b';

    return modeString;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
	return nullptr;
}

std::string GetLocalFilePath(const std::string& filename)
{
	return "";
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool CloseFile(FileHandle* file)
{
    return false;
}

bool IsEndOfFile(FileHandle* file)
{
    return true;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return false;
}

bool FileExists(const std::string& name)
{
	return false;
}

bool LocalFileExists(const std::string& name)
{
    return false;
}

bool CheckFileWritable(const std::string& filepath)
{
	return false;
}

bool CheckLocalFileWritable(const std::string& name)
{
    return false;
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    return false;
}

void FileRewind(FileHandle* file)
{
}

u64 FilePosition(FileHandle* file)
{
    return 0;
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return 0;
}

bool FileFlush(FileHandle* file)
{
    return false;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return 0;
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    return 0;
}

u64 FileLength(FileHandle* file)
{
    return 0;
}

void Log(LogLevel level, const char* fmt, ...)
{
#ifdef DEBUG
	if (fmt == nullptr)
		return;

	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
#endif
}

// XPCog: threading and timing on the C++ standard library rather than pthreads.
//
// Cog's version of this file is POSIX throughout -- pthread_create, a
// condition-variable semaphore, usleep, clock_gettime -- which is unremarkable
// for a player that ships on macOS alone and does not build at all under MSVC.
// The primitives melonDS asks for map onto <thread>, <mutex> and <chrono>
// one for one, so the standard library is both the portable answer and the
// shorter one.
//
// Note that in this build none of it does much work: the ARM recompiler and
// the threaded software renderer are both off, so the emulator runs on the
// calling thread and these exist to be linked against rather than used.

Thread* Thread_Create(std::function<void()> func)
{
    return reinterpret_cast<Thread*>(new std::thread(std::move(func)));
}

void Thread_Free(Thread* thread)
{
    auto* t = reinterpret_cast<std::thread*>(thread);
    if (!t) return;
    // Joinable is checked rather than assumed: Thread_Wait may already have
    // joined, and std::thread throws where pthread_join merely misbehaved.
    if (t->joinable()) t->join();
    delete t;
}

void Thread_Wait(Thread* thread)
{
    auto* t = reinterpret_cast<std::thread*>(thread);
    if (t && t->joinable()) t->join();
}

// A counting semaphore, which C++ did not have until <semaphore> in C++20.
struct PlatformSemaphore
{
    std::mutex              lock;
    std::condition_variable bump;
    unsigned                count = 0;
};

Semaphore* Semaphore_Create()
{
    return reinterpret_cast<Semaphore*>(new PlatformSemaphore);
}

void Semaphore_Free(Semaphore* sema)
{
    delete reinterpret_cast<PlatformSemaphore*>(sema);
}

void Semaphore_Reset(Semaphore* sema)
{
    auto* sem = reinterpret_cast<PlatformSemaphore*>(sema);
    std::lock_guard<std::mutex> guard(sem->lock);
    sem->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    auto* sem = reinterpret_cast<PlatformSemaphore*>(sema);
    std::unique_lock<std::mutex> guard(sem->lock);
    sem->bump.wait(guard, [sem] { return sem->count > 0; });
    --sem->count;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    auto* sem = reinterpret_cast<PlatformSemaphore*>(sema);
    std::unique_lock<std::mutex> guard(sem->lock);
    if (!sem->bump.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                            [sem] { return sem->count > 0; }))
        return false;
    --sem->count;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    auto* sem = reinterpret_cast<PlatformSemaphore*>(sema);
    {
        std::lock_guard<std::mutex> guard(sem->lock);
        sem->count += static_cast<unsigned>(count);
    }
    // Post takes a count, so more than one waiter can be released.
    sem->bump.notify_all();
}

Mutex* Mutex_Create()
{
    return reinterpret_cast<Mutex*>(new std::mutex);
}

void Mutex_Free(Mutex* mutex)
{
    delete reinterpret_cast<std::mutex*>(mutex);
}

void Mutex_Lock(Mutex* mutex)
{
    reinterpret_cast<std::mutex*>(mutex)->lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    reinterpret_cast<std::mutex*>(mutex)->unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return reinterpret_cast<std::mutex*>(mutex)->try_lock();
}

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

// steady_clock is the monotonic one, which is what CLOCK_MONOTONIC was for:
// these feed frame pacing, so a wall clock that can step backwards will not do.
u64 GetMSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

u64 GetUSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}


void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata)
{
}


void MP_Begin(void* userdata)
{
}

void MP_End(void* userdata)
{
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
	return -1;
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
	return -1;
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
	return -1;
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
	return -1;
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
	return -1;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
	return -1;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
	return -1;
}


int Net_SendPacket(u8* data, int len, void* userdata)
{
	return 0;
}

int Net_RecvPacket(u8* data, void* userdata)
{
	return 0;
}


void Mic_Start(void* userdata)
{
}

void Mic_Stop(void* userdata)
{
}

int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
	memset(data, 0, maxlength * sizeof(s16));
}


void Camera_Start(int num, void* userdata)
{
}

void Camera_Stop(int num, void* userdata)
{
}

void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata)
{
}

bool Addon_KeyDown(KeyType type, void* userdata)
{
	return false;
}

void Addon_RumbleStart(u32 len, void* userdata)
{
}

void Addon_RumbleStop(void* userdata)
{
}

float Addon_MotionQuery(MotionQueryType type, void* userdata)
{
	return 0;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib)
{
	return nullptr;
}

void DynamicLibrary_Unload(DynamicLibrary* lib)
{
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
	return nullptr;
}

}
