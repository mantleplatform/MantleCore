// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptSide.h"

const TCHAR* MantleSideName(EMantleSide Side)
{
	switch (Side)
	{
	case EMantleSide::Server: return TEXT("server");
	case EMantleSide::Client: return TEXT("client");
	case EMantleSide::Both: return TEXT("server and client");
	default: return TEXT("none");
	}
}
