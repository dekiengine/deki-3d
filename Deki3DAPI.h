#pragma once

// Defines only DEKI_3D_API. Package headers include this, not
// Deki3DPackage.h: that umbrella header pulls in every header of the
// package, so including it from one of them would be circular.

#ifdef DEKI_EDITOR
#ifdef _WIN32
#ifdef DEKI_3D_EXPORTS
#define DEKI_3D_API __declspec(dllexport)
#else
#define DEKI_3D_API __declspec(dllimport)
#endif
#else
#define DEKI_3D_API
#endif
#else
#define DEKI_3D_API
#endif
