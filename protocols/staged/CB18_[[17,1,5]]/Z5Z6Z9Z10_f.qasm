OPENQASM 3.0;
include "stdgates.inc";

// Flagged syndrome extraction for Z5Z6Z9Z10.
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

// Interleaved data-to-syndrome and flag-to-syndrome CNOTs.
cx data[4], syn;
cx flag[0], syn;
cx data[5], syn;
cx data[8], syn;
cx flag[0], syn;
cx data[9], syn;

// Syndrome measurement in Z basis.
m = measure syn;

// Flag measurements in X basis.
h flag[0];
f[0] = measure flag[0];
