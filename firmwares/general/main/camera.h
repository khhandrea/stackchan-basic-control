// GC0308 camera (DVP, SCCB on the internal I2C bus). Methods: camera.snapshot, camera.set_resolution.
#pragma once

namespace camera {

void init();
void registerMethods();

}  // namespace camera
