#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** File dialogs and messages for project New / Open / Save / Save As (message thread only). */
namespace projectCommands
{
    void newProject (AppState&);
    void openProject (AppState&);
    void save (AppState&);       // Save As when the project has no file yet
    void saveAs (AppState&);

    /** After an open: offers backup restore if the last session crashed, warns about missing media. */
    void reviewOpenedProject (AppState&);
}

} // namespace b4nd
