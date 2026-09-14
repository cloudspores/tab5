// One-time update of the ESP32-C6 co-processor firmware over the hosted link.
#pragma once
namespace c6 {
// Call after esp_hosted_connect_to_slave(). If the co-processor runs a legacy (pre-2.x)
// image, pushes the embedded image, activates it, and restarts the host. Never returns
// in that case. Returns immediately when no update is needed.
void update_if_needed();
}
