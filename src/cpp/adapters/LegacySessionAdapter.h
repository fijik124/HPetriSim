#pragma once
#include "application/DocumentSimulationSession.h"

class CPetriSimDoc;

namespace hps::adapters
{
// Must run on the document-owning thread while legacy simulation is offline.
// Copies current model and exact clock/settings. No document pointer is retained.
application::SessionCreationResult CreateLegacyDocumentSession(CPetriSimDoc& document,
    application::DocumentId identity, std::uint32_t seed = 1);
}
