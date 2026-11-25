#pragma once
class mcCarSim;
// Normalized current native boost FOV, relative to configured peak.
// False means use the original independent effect envelopes.
bool GetBoostFOVEnvelope(const mcCarSim* sim, float& nitro, float& sst);
