#include "stdafx.h"
#include <catch2/catch_session.hpp>
#include <iostream>

int main(int argc, char* argv[])
{
	// PetriSim.cpp supplies the real application and GDI+ lifetime objects.
	// Initialize MFC for this console process, without calling InitInstance:
	// no ribbon, printer discovery, document windows or worker threads.
	if (!AfxWinInit(GetModuleHandle(nullptr), nullptr, GetCommandLine(), 0))
	{
		std::cerr << "MFC initialization failed\n";
		return 1;
	}
	return Catch::Session().run(argc, argv);
}
