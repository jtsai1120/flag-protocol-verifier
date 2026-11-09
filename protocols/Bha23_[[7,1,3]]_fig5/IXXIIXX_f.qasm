OPENQASM 3.0;
include "stdgates.inc";

// Syndrome extraction for Steane [[7,1,3]] stabilizer IXXIIXX.
// Controlled-P normal form: qm[0] is the syndrome ancilla, prepared in |+>
// and read in the X basis (final H shown).
// qf[0] is the flag ancilla, initialized in |0> and read in the Z basis.
// The two syndrome-flag couplings use the standard distance-3 flag gadget:
// after the first data interaction and before the last data interaction.
qubit[7] qd;
qubit[1] qm;
qubit[1] qf;
bit[1] cm;
bit[1] cf;

h qm[0];

// X on qd[1]
cx qm[0], qd[1];

// First flag coupling
cx qm[0], qf[0];

// X on qd[2]
cx qm[0], qd[2];

// X on qd[5]
cx qm[0], qd[5];

// Second flag coupling
cx qm[0], qf[0];

// X on qd[6]
cx qm[0], qd[6];

h qm[0];

// Syndrome readout: the final H above rotated the X-basis value into
// the Z basis, so these are plain measurements.
cm[0] = measure qm[0];

// Flag readout: qf starts in |0> and is read in the Z basis directly.
cf[0] = measure qf[0];
