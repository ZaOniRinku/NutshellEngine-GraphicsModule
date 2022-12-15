#pragma once
#include "../external/Common/module_interfaces/ntsh_graphics_module_interface.h"
#if defined(NTSH_OS_WINDOWS)
#include <windows.h>
#elif defined(NTSH_OS_LINUX)
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif
#include "../external/glad/include/glad/glad.h"

#if defined(NTSH_OS_LINUX)
typedef GLXContext(*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
#endif

class NutshellGraphicsModule : public NutshellGraphicsModuleInterface {
public:
	NutshellGraphicsModule() : NutshellGraphicsModuleInterface("Nutshell Graphics Test Module") {}

	void init();
	void update(double dt);
	void destroy();

private:
#if defined(NTSH_OS_WINDOWS)
	HDC m_deviceContext;

	HGLRC m_context;
#elif defined(NTSH_OS_LINUX)
	Display* m_display;

	GLXContext m_context;
#endif
};