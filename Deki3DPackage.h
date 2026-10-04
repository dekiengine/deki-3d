#pragma once

// Umbrella include for deki-3d. Include this from outside the package; headers
// inside it include Deki3DAPI.h instead, to avoid a circular include.
//
// Component headers sit behind their feature's define, so a build that strips
// a feature does not name what it did not compile. See COMPATIBILITY.md on
// stripping.

#include "Deki3DAPI.h"

#ifdef DEKI_PACKAGE_3D

// Geometry and maths carry no components and are always present: the
// rasteriser needs them whatever the feature set.
#include "Math3D.h"
#include "Mesh3D.h"

#ifndef DEKI_PACKAGE_FEATURES_CONFIGURED
// A build that has not chosen its features gets all of them, which is what
// the editor always wants and what a project gets before it configures.
#define DEKI_FEATURE_3D_SOFTWARE
#endif

#ifdef DEKI_FEATURE_3D_SOFTWARE
#include "Mesh3DSettings.h"
#include "Mesh3DPass.h"
#include "MeshComponent.h"
#include "Raster3D.h"
#endif

#endif  // DEKI_PACKAGE_3D
