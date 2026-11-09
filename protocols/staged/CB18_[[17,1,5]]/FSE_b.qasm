OPENQASM 3.0;
include "stdgates.inc";

// Combined unflagged syndrome extraction for all 16 generators
// of the [[17,1,5]] color code, in Table-7 order.

// data[0] = q1, ..., data[16] = q17.
// syn = measurement ancilla, reused for every generator.

qubit[17] data;
qubit syn;

bit[16] m;


// ------------------------------------------------------------
// Unflagged SE for G0 = Z1Z2Z3Z4
// ------------------------------------------------------------
reset syn;

cx data[0], syn;
cx data[1], syn;
cx data[2], syn;
cx data[3], syn;

m[0] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G1 = X1X2X3X4
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[0];
h data[1];
h data[2];
h data[3];

cx data[0], syn;
cx data[1], syn;
cx data[2], syn;
cx data[3], syn;

// Restore data-qubit basis.
h data[0];
h data[1];
h data[2];
h data[3];

m[1] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G2 = Z1Z3Z5Z6
// ------------------------------------------------------------
reset syn;

cx data[0], syn;
cx data[2], syn;
cx data[4], syn;
cx data[5], syn;

m[2] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G3 = X1X3X5X6
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[0];
h data[2];
h data[4];
h data[5];

cx data[0], syn;
cx data[2], syn;
cx data[4], syn;
cx data[5], syn;

// Restore data-qubit basis.
h data[0];
h data[2];
h data[4];
h data[5];

m[3] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G4 = Z5Z6Z9Z10
// ------------------------------------------------------------
reset syn;

cx data[4], syn;
cx data[5], syn;
cx data[8], syn;
cx data[9], syn;

m[4] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G5 = X5X6X9X10
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[4];
h data[5];
h data[8];
h data[9];

cx data[4], syn;
cx data[5], syn;
cx data[8], syn;
cx data[9], syn;

// Restore data-qubit basis.
h data[4];
h data[5];
h data[8];
h data[9];

m[5] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G6 = Z7Z8Z11Z12
// ------------------------------------------------------------
reset syn;

cx data[6], syn;
cx data[7], syn;
cx data[10], syn;
cx data[11], syn;

m[6] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G7 = X7X8X11X12
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[6];
h data[7];
h data[10];
h data[11];

cx data[6], syn;
cx data[7], syn;
cx data[10], syn;
cx data[11], syn;

// Restore data-qubit basis.
h data[6];
h data[7];
h data[10];
h data[11];

m[7] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G8 = Z9Z10Z13Z14
// ------------------------------------------------------------
reset syn;

cx data[8], syn;
cx data[9], syn;
cx data[12], syn;
cx data[13], syn;

m[8] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G9 = X9X10X13X14
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[8];
h data[9];
h data[12];
h data[13];

cx data[8], syn;
cx data[9], syn;
cx data[12], syn;
cx data[13], syn;

// Restore data-qubit basis.
h data[8];
h data[9];
h data[12];
h data[13];

m[9] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G10 = Z11Z12Z15Z16
// ------------------------------------------------------------
reset syn;

cx data[10], syn;
cx data[11], syn;
cx data[14], syn;
cx data[15], syn;

m[10] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G11 = X11X12X15X16
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[10];
h data[11];
h data[14];
h data[15];

cx data[10], syn;
cx data[11], syn;
cx data[14], syn;
cx data[15], syn;

// Restore data-qubit basis.
h data[10];
h data[11];
h data[14];
h data[15];

m[11] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G12 = Z8Z12Z16Z17
// ------------------------------------------------------------
reset syn;

cx data[7], syn;
cx data[11], syn;
cx data[15], syn;
cx data[16], syn;

m[12] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G13 = X8X12X16X17
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[7];
h data[11];
h data[15];
h data[16];

cx data[7], syn;
cx data[11], syn;
cx data[15], syn;
cx data[16], syn;

// Restore data-qubit basis.
h data[7];
h data[11];
h data[15];
h data[16];

m[13] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G14 = Z3Z4Z6Z7Z10Z11Z14Z15
// ------------------------------------------------------------
reset syn;

cx data[2], syn;
cx data[3], syn;
cx data[5], syn;
cx data[6], syn;
cx data[9], syn;
cx data[10], syn;
cx data[13], syn;
cx data[14], syn;

m[14] = measure syn;
barrier data, syn;

// ------------------------------------------------------------
// Unflagged SE for G15 = X3X4X6X7X10X11X14X15
// ------------------------------------------------------------
reset syn;

// Change support qubits from X-basis parity to Z-style parity extraction.
h data[2];
h data[3];
h data[5];
h data[6];
h data[9];
h data[10];
h data[13];
h data[14];

cx data[2], syn;
cx data[3], syn;
cx data[5], syn;
cx data[6], syn;
cx data[9], syn;
cx data[10], syn;
cx data[13], syn;
cx data[14], syn;

// Restore data-qubit basis.
h data[2];
h data[3];
h data[5];
h data[6];
h data[9];
h data[10];
h data[13];
h data[14];

m[15] = measure syn;
