#pragma once

// Tiny header that defines only DEKI_3D_API. Package headers include
// this instead of Deki3DPackage.h to avoid a circular include
// (Deki3DPackage.h is the umbrella include for external consumers and
// pulls in every header of the package, so including it from one of them would
// re-enter the file currently being defined).

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
