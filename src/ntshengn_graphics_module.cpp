#include "ntshengn_graphics_module.h"
#include "../external/Module/utils/ntshengn_module_defines.h"
#include "../external/Module/utils/ntshengn_dynamic_library.h"
#include "../external/Common/utils/ntshengn_defines.h"
#include "../external/Common/utils/ntshengn_enums.h"

void NtshEngn::GraphicsModule::init() {
	NTSHENGN_MODULE_FUNCTION_NOT_IMPLEMENTED();
}

void NtshEngn::GraphicsModule::update(double dt) {
	NTSHENGN_UNUSED(dt);
	NTSHENGN_MODULE_FUNCTION_NOT_IMPLEMENTED();
}

void NtshEngn::GraphicsModule::destroy() {
	NTSHENGN_MODULE_FUNCTION_NOT_IMPLEMENTED();
}

NtshEngn::MeshId NtshEngn::GraphicsModule::load(const NtshEngn::Mesh mesh) {
	NTSHENGN_MODULE_FUNCTION_NOT_IMPLEMENTED();
	return 0;
}

NtshEngn::ImageId NtshEngn::GraphicsModule::load(const NtshEngn::Image mesh) {
	NTSHENGN_MODULE_FUNCTION_NOT_IMPLEMENTED();
	return 0;
}

extern "C" NTSHENGN_MODULE_API NtshEngn::GraphicsModuleInterface* createModule() {
	return new NtshEngn::GraphicsModule;
}

extern "C" NTSHENGN_MODULE_API void destroyModule(NtshEngn::GraphicsModuleInterface* m) {
	delete m;
}