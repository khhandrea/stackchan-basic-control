// Modal message box over the face. Methods: modal.get, modal.show, modal.close. Event: modal.result.
#pragma once

#include "gfx.h"

namespace modal {

void registerMethods();

// Display task, once per frame: handles the timeout. Returns true if a modal is visible.
bool update(int64_t now_ms);
void render(const gfx::Band& b);

// Screen tap at (x, y). Returns true if the modal consumed it.
bool handleTap(int x, int y);

}  // namespace modal
