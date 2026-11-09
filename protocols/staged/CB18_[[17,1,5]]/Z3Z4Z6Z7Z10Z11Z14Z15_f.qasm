OPENQASM 3.0;
include "stdgates.inc";

// Flagged syndrome extraction for Z3Z4Z6Z7Z10Z11Z14Z15.
// Based on the flag-circuit patterns in arXiv:1708.02246:
// weight-4 generators use one flag qubit; weight-8 generators use three flag qubits.

// data[0] = q1, ..., data[16] = q17.
// syn = measurement ancilla, initialized to |0>.
// flag[j] = flag ancilla, initialized to |+> and measured in X basis.

qubit[17] data;
qubit syn;
qubit[3] flag;

bit m;
bit[3] f;

reset syn;
reset flag[0];
h flag[0];
reset flag[1];
h flag[1];
reset flag[2];
h flag[2];

// Interleaved data-to-syndrome and flag-to-syndrome CNOTs.
cx data[2], syn;
cx flag[0], syn;
cx data[3], syn;
cx flag[1], syn;
cx data[5], syn;
cx flag[2], syn;
cx data[6], syn;
cx data[9], syn;
cx flag[0], syn;
cx data[10], syn;
cx flag[1], syn;
cx data[13], syn;
cx flag[2], syn;
cx data[14], syn;

// Syndrome measurement in Z basis.
m = measure syn;

// Flag measurements in X basis.
h flag[0];
f[0] = measure flag[0];
h flag[1];
f[1] = measure flag[1];
h flag[2];
f[2] = measure flag[2];
