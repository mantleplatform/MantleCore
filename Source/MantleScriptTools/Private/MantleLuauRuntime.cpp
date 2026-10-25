// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLuauRuntime.h"

#include "Containers/StringConv.h"
#include "HAL/UnrealMemory.h"

#include <cstdlib>

THIRD_PARTY_INCLUDES_START
#include "lua.h"
// Luau's public buffer struct contains an opaque `struct TString*`. Unreal also exposes a
// global TString alias template on Linux, where C++ tag names share that namespace. Rename
// only the opaque declaration while parsing this header; its pointer-sized ABI is unchanged.
#define TString LuauOpaqueTString
#include "lualib.h"
#undef TString
#include "luacode.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	struct FLuauMemory
	{
		uint64 UsedBytes = 0;
		uint64 LimitBytes = MAX_uint64;
		bool bExceeded = false;
	};

	void* LuauAllocate(void* Userdata, void* Pointer, size_t OldSize, size_t NewSize)
	{
		FLuauMemory& Memory = *static_cast<FLuauMemory*>(Userdata);
		const uint64 OldBytes = static_cast<uint64>(OldSize);
		const uint64 NewBytes = static_cast<uint64>(NewSize);

		if (NewSize == 0)
		{
			FMemory::Free(Pointer);
			Memory.UsedBytes = OldBytes <= Memory.UsedBytes ? Memory.UsedBytes - OldBytes : 0;
			return nullptr;
		}

		const uint64 BaseBytes = OldBytes <= Memory.UsedBytes ? Memory.UsedBytes - OldBytes : 0;
		if (NewBytes > Memory.LimitBytes || BaseBytes > Memory.LimitBytes - NewBytes)
		{
			Memory.bExceeded = true;
			return nullptr;
		}

		void* Result = FMemory::Realloc(Pointer, NewSize);
		if (Result == nullptr)
		{
			Memory.bExceeded = true;
			return nullptr;
		}

		Memory.UsedBytes = BaseBytes + NewBytes;
		return Result;
	}

	FString ErrorFromStack(lua_State* State)
	{
		const char* Message = lua_tostring(State, -1);
		return Message != nullptr ? UTF8_TO_TCHAR(Message) : TEXT("unknown Luau error");
	}
}

bool FMantleLuauRuntime::ValidateSource(
	const FString& Source,
	FMantleScriptError& OutError,
	uint64 MaximumSourceBytes)
{
	FTCHARToUTF8 Utf8Source(*Source);
	if (static_cast<uint64>(Utf8Source.Length()) > MaximumSourceBytes)
	{
		OutError = {EMantleScriptErrorKind::Compile, TEXT("Luau source exceeds the configured source limit")};
		return false;
	}

	FLuauMemory Memory;
	lua_State* State = lua_newstate(&LuauAllocate, &Memory);
	if (State == nullptr)
	{
		OutError = {EMantleScriptErrorKind::MemoryBudgetExceeded, TEXT("unable to allocate Luau validation state")};
		return false;
	}

	size_t BytecodeSize = 0;
	char* Bytecode = luau_compile(
		Utf8Source.Get(), static_cast<size_t>(Utf8Source.Length()), nullptr, &BytecodeSize);
	if (Bytecode == nullptr)
	{
		lua_close(State);
		OutError = {EMantleScriptErrorKind::Compile, TEXT("Luau compiler failed to return bytecode")};
		return false;
	}

	const int32 LoadResult = luau_load(State, "MantleEditorValidation", Bytecode, BytecodeSize, 0);
	std::free(Bytecode);
	if (LoadResult != 0)
	{
		OutError = {EMantleScriptErrorKind::Compile, ErrorFromStack(State)};
		lua_close(State);
		return false;
	}

	lua_close(State);
	OutError = FMantleScriptError();
	return true;
}
