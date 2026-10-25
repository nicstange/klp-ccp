/*
 * B gets both of its x references lowered, so its rewritten
 * replacement list doesn't reference the parameter anymore and B's
 * argument source is not subject to macro expansion. A's first x
 * reference lies within that argument and thus must not get lowered
 * there, the lowered source would never get expanded. It gets
 * replaced verbatim and the second reference gets lowered.
 */
int espr_i;
int espr_j;
int espr_k;

#define FOO espr_i
#define BAZ espr_j
#define QUX espr_k
#define B(x) (({ int espr_i = 1; x; }) + ({ int espr_j = 1; x; }))
#define A(x) (B(x) + ({ int espr_k = 1; x; }))

int pu_f(void)
{
	return A(FOO + BAZ + QUX);
}
