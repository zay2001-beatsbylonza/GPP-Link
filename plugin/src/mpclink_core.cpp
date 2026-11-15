// The SDK builds every .cpp in src/. This pulls in the shared transfer core
// (plugin-core/mpclink.c, plain C that also compiles as C++) so the plugin and
// the mpclink-cli test tool use exactly the same code.
#include "../../plugin-core/mpclink.c"
