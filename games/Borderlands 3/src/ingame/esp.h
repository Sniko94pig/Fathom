#pragma once
#include "core/model.h"

namespace esp {

void Draw(core::Model& m);
void SetLiveCamera(void* manager, int povOffset, float focalScale);
void ClearLiveCamera();

}
