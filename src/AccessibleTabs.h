#pragma once

// Qt's built-in tab accessibility omits the real tab buttons (including close).
// Install before creating tab bars, for both browser and terminal tabs on all OSes.
void installAccessibleTabs();
