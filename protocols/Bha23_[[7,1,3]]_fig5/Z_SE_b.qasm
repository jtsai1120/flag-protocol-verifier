OPENQASM 3.0;
include "stdgates.inc";

// Bhatnagar et al., Steane [[7,1,3]], Fig. 5.
// Combined bare Z-type syndrome extraction.
// qm[0] -> IIIZZZZ
// qm[1] -> IZZIIZZ
// qm[2] -> ZIZIZIZ
// Measurement ancillas are prepared in |+> and returned to the Z basis
// with a final H, so a plain Z-basis measurement reads them.
qubit[7] qd;
qubit[3] qm;
bit[3] cm;

// IIIZZZZ
h qm[0];
cz qm[0], qd[3];
cz qm[0], qd[4];
cz qm[0], qd[5];
cz qm[0], qd[6];
h qm[0];

// IZZIIZZ
h qm[1];
cz qm[1], qd[1];
cz qm[1], qd[2];
cz qm[1], qd[5];
cz qm[1], qd[6];
h qm[1];

// ZIZIZIZ
h qm[2];
cz qm[2], qd[0];
cz qm[2], qd[2];
cz qm[2], qd[4];
cz qm[2], qd[6];
h qm[2];

// Syndrome readout: the final H above rotated the X-basis value into
// the Z basis, so these are plain measurements.
cm[0] = measure qm[0];
cm[1] = measure qm[1];
cm[2] = measure qm[2];
