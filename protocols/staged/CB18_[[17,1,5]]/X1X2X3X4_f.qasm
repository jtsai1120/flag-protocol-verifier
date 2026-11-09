OPENQASM 3.0;
include "stdgates.inc";

// Flagged syndrome extraction for X1X2X3X4.
// Based on the flag-circuit patterns in arXiv:1708.02246:
// weight-4 generators use one flag qubit; weight-8 generators use three flag qubits.

// data[0] = q1, ..., data[16] = q17.
// syn = measurement ancilla, initialized to |0>.
// flag[j] = flag ancilla, initialized to |+> and measured in X basis.

qubit[17] data;
qubit syn;
qubit[1] flag;

bit m;
bit[1] f;

reset syn;
reset flag[0];
h flag[0];

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[0];
h data[1];
h data[2];
h data[3];

// Interleaved data-to-syndrome and flag-to-syndrome CNOTs.
cx data[0], syn;
cx flag[0], syn;
cx data[1], syn;
cx data[2], syn;
cx flag[0], syn;
cx data[3], syn;

// Restore data-qubit basis.
h data[0];
h data[1];
h data[2];
h data[3];

// Syndrome measurement in Z basis.
m = measure syn;

// Flag measurements in X basis.
h flag[0];
f[0] = measure flag[0];
