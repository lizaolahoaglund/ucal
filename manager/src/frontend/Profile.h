#pragma once
#include "../control/FrontendPlugins.h"
namespace Ucal::Manager {
const char* frontendImplementation();
std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage&);
}
