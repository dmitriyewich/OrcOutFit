#pragma once

// SilentPatch 1.1.34 resolves the frontend flag through the operand at 0x53E9AD.
// Poll briefly during startup so a later inline CALL installed at 0x53E9AC cannot
// turn that operand into a dangling rel32 displacement.
// Returns true only while another startup poll can still be useful.
bool OrcSilentPatchCompatPoll();
