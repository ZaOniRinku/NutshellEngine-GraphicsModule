#include "ntsh_graphics_module.h"
#include "../external/Module/utils/ntsh_module_defines.h"
#include "../external/Module/utils/ntsh_dynamic_library.h"
#include "../external/Common/utils/ntsh_engine_defines.h"
#include "../external/Common/utils/ntsh_engine_enums.h"
#include "../external/Common/module_interfaces/ntsh_window_module_interface.h"

void NutshellGraphicsModule::init() {
	if (!m_windowModule) {
		NTSH_MODULE_ERROR("Headless rendering is not supported.", NtshResult::ModuleError);
	}

#ifdef NTSH_OS_WINDOWS
	m_deviceContext = GetDC(m_windowModule->getNativeHandle());

	PIXELFORMATDESCRIPTOR pixelFormatDescriptor;
	pixelFormatDescriptor.nSize = sizeof(PIXELFORMATDESCRIPTOR);
	pixelFormatDescriptor.nVersion = 1;
	pixelFormatDescriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pixelFormatDescriptor.iPixelType = PFD_TYPE_RGBA;
	pixelFormatDescriptor.cColorBits = 32;
	pixelFormatDescriptor.cRedBits = 0;
	pixelFormatDescriptor.cRedShift = 0;
	pixelFormatDescriptor.cGreenBits = 0;
	pixelFormatDescriptor.cGreenShift = 0;
	pixelFormatDescriptor.cBlueBits = 0;
	pixelFormatDescriptor.cBlueShift = 0;
	pixelFormatDescriptor.cAlphaBits = 0;
	pixelFormatDescriptor.cAlphaShift = 0;
	pixelFormatDescriptor.cAccumBits = 0;
	pixelFormatDescriptor.cAccumRedBits = 0;
	pixelFormatDescriptor.cAccumGreenBits = 0;
	pixelFormatDescriptor.cAccumBlueBits = 0;
	pixelFormatDescriptor.cAccumAlphaBits = 0;
	pixelFormatDescriptor.cDepthBits = 24;
	pixelFormatDescriptor.cStencilBits = 8;
	pixelFormatDescriptor.cAuxBuffers = 0;
	pixelFormatDescriptor.iLayerType = PFD_MAIN_PLANE;
	pixelFormatDescriptor.bReserved = 0;
	pixelFormatDescriptor.dwLayerMask = 0;
	pixelFormatDescriptor.dwVisibleMask = 0;
	pixelFormatDescriptor.dwDamageMask = 0;
	int pixelFormat = ChoosePixelFormat(m_deviceContext, &pixelFormatDescriptor);
	if (pixelFormat == 0) {
		NTSH_MODULE_ERROR("Could not find a suitable pixel format.", NtshResult::ModuleError);
	}

	DescribePixelFormat(m_deviceContext, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pixelFormatDescriptor);
	pixelFormat = ChoosePixelFormat(m_deviceContext, &pixelFormatDescriptor);
	if (pixelFormat == 0) {
		NTSH_MODULE_ERROR("Could not find a suitable pixel format.", NtshResult::ModuleError);
	}

	if (!SetPixelFormat(m_deviceContext, pixelFormat, &pixelFormatDescriptor)) {
		NTSH_MODULE_ERROR("Could not set the pixel format. Error code: " + std::to_string(GetLastError()), NtshResult::ModuleError);
	}

	m_context = wglCreateContext(m_deviceContext);
	if (!m_context) {
		NTSH_MODULE_ERROR("Could not create an OpenGL context. Error code: " + std::to_string(GetLastError()), NtshResult::ModuleError);
	}

	wglMakeCurrent(m_deviceContext, m_context);
#elif NTSH_OS_LINUX
	m_display = XOpenDisplay(NULL);

	static int visualAttributes[] = {
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_DOUBLEBUFFER, true,
		GLX_RED_SIZE, 1,
		GLX_GREEN_SIZE, 1,
		GLX_BLUE_SIZE, 1,
		None
	};

	int* framebufferConfigsCount = 0;
	GLXFBConfig* framebufferConfigs = glXChooseFBConfig(m_display, DefaultScreen(m_display), visualAttributes, &framebufferConfigsCount);
	if (!framebufferConfigs) {
		NTSH_MODULE_ERROR("Could not choose a framebuffer config.", NtshResult::ModuleError);
	}

	glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 0;
	glXCreateContextAttribsARB = (glXCreateContextAttribsARBProc)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
	if (!glXCreateContextAttribsARB) {
		NTSH_MODULE_ERROR("Could not load glXCreateContextAttribsARB().", NtshResult::ModuleError);
	}

	static int contextAttributes[] = {
		GLX_CONTENT_MAJOR_VERSION_ARB, 4,
		GLX_CONTENT_MINOR_VERSION_ARB, 2,
		None
	};

	m_context = glXCreateContextAttribsARB(m_display, framebufferConfigs[0], NULL, true, contextAttributes);
	if (!m_context) {
		NTSH_MODULE_ERROR("Could not create an OpenGL context.", NtshResult::ModuleError);
	}

	XMapWindow(m_display, m_windowModule->getNativeHandle());
	glXMakeCurrent(m_display, m_windowModule->getNativeHandle(), m_context);
#endif

	if (!gladLoadGL()) {
		NTSH_MODULE_ERROR("Cannot load OpenGL functions.", NtshResult::ModuleError);
	}
}

void NutshellGraphicsModule::update(double dt) {
	NTSH_UNUSED(dt);
	NTSH_MODULE_FUNCTION_NOT_IMPLEMENTED();
}

void NutshellGraphicsModule::destroy() {
	wglMakeCurrent(m_deviceContext, NULL);
	wglDeleteContext(m_context);
}

extern "C" NTSH_MODULE_API NutshellGraphicsModuleInterface* createModule() {
	return new NutshellGraphicsModule;
}

extern "C" NTSH_MODULE_API void destroyModule(NutshellGraphicsModuleInterface* m) {
	delete m;
}