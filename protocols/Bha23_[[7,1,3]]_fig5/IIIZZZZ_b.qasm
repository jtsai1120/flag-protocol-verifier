OPENQASM 3.0;
include "stdgates.inc";

// Syndrome extraction for Steane [[7,1,3]] stabilizer IIIZZZZ.
// Controlled-P normal form: qm[0] is the syndrome ancilla, prepared in |+>
// and read in the X basis (final H shown).
qubit[7] qd;
qubit[1] qm;
bit[1] cm;

h qm[0];

// Z on qd[3]
cz qm[0], qd[3];

// Z on qd[4]
cz qm[0], qd[4];

// Z on qd[5]
cz qm[0], qd[5];

// Z on qd[6]
cz qm[0], qd[6];

h qm[0];

// Syndrome readout: the final H above rotated the X-basis value into
// the Z basis, so these are plain measurements.
cm[0] = measure qm[0];
