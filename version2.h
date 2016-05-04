#pragma once

#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)

#define FFDRIVER_VERSION_MAJOR		  1
#define FFDRIVER_VERSION_MINOR		  7
#define FFDRIVER_VERSION_REVISION		0
#define FFDRIVER_VERSION_BUILD	  	0

#define VER_FILE_VERSION            FFDRIVER_VERSION_MAJOR, FFDRIVER_VERSION_MINOR, FFDRIVER_VERSION_REVISION, FFDRIVER_VERSION_BUILD
#define VER_FILE_VERSION_STR        STRINGIZE(FFDRIVER_VERSION_MAJOR)        \
	"." STRINGIZE(FFDRIVER_VERSION_MINOR)    \
	"." STRINGIZE(FFDRIVER_VERSION_REVISION) \
	"." STRINGIZE(FFDRIVER_VERSION_BUILD)    \

#define VER_PRODUCT_VERSION         VER_FILE_VERSION
#define VER_PRODUCT_VERSION_STR     VER_FILE_VERSION_STR


