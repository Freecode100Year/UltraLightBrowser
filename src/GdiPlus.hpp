#pragma once

// gdiplus.h relies on min/max macros, which NOMINMAX removes project-wide.
#include <windows.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <objidl.h>
#include <gdiplus.h>
