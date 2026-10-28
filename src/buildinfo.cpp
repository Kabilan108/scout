#include "logging.h"

// Alone in its own file so a new commit only recompiles this.
const char *logging::commit() { return SCOUT_COMMIT; }
