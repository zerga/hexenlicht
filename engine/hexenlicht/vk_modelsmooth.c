/* vk_modelsmooth.c -- smoother alias model animation (story 8.2)
 *
 * An MDL stores each pose vertex as one byte per axis on one grid for the
 * whole model, rounded on its own in every frame, and a normal that is one
 * of Quake's 162 directions (anorms.h): rigid parts shake by up to half a
 * grid step between frames and the shading jumps (8.1, DECISIONS G14).
 * When vk_model.c builds a model, VK_ModelPoses makes its poses for
 * model_geometry.comp:
 * - The model's vertices: gl_mesh.c's command vertices (a vertex once for
 *   every strip that uses it; the file's own numbering is gone after the
 *   load) that are the same in every pose, bytes and table normal; seam
 *   duplicates differ in their one-sided table normals and stay apart
 *   (file vertices that coincide with equal normals become one).
 * - r_smoothmodels 1: per sequence (frames named alike but for a trailing
 *   number; a frame group's subframes), each vertex's path on each axis the
 *   smoothest one (least squared second differences, across the wrap for a
 *   loop: a frame group, or the last pose within 1.5x the mean
 *   frame-to-frame distance of the first) within 0.49 grid steps of its
 *   byte, so rounding gives back the original: tools/hexenlicht/
 *   mdl_smooth.cs's ADMM, ported; and normals rebuilt from the smoothed
 *   shape (each face's unit normal counts the same, as the tables were
 *   made; a face under a quarter of a grid cell by its area; of faces on the
 *   same three vertices the one that agrees with the table over all poses;
 *   a vertex whose faces weigh less than one full face takes the rest from
 *   its table normal). A model whose rebuilt normals change between frames
 *   more than its table's keeps the table's (8.1: some effects).
 *   r_smoothmodels 0: the bytes and the table normals, GL's poses.
 * - r_smoothseams 1: vertices at the same place in every pose (a UV
 *   seam's duplicates) share one normal, but not a card's two sides
 *   (faces that lie on each other only once welded, or table normals that
 *   point against each other); 0: each its own side, the original game's.
 * - Packed in 8 bytes: x, y, z in 12 bits ((grid steps + 0.5) x 16: a byte
 *   exactly; a byte of 255 smoothed above 255.47 clamps at 255.44), the
 *   normal in 14 + 14 bits, octahedral (model_geometry.comp's
 *   pose_position and pose_normal).
 * The solved positions are kept for the session (by name and pose data),
 * so a map change or a switch of the settings doesn't solve again.
 * "vk_models smooth <model>" prints mdl_smooth.ps1's numbers for the
 * engine's own result.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2026  Hexenlicht contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 */

#include "quakedef.h"
#include "vk_local.h"
#include "shaders/hl_shared.h"
#include <windows.h>

/* Quake's vertex normals, indexed by trivertx_t.lightnormalindex */
static const float vertex_normals[NUM_VERTEX_NORMALS][3] =
{
#include "anorms.h"
};

static void SmoothChanged (cvar_t *var);

static cvar_t	r_smoothmodels = {"r_smoothmodels", "1", CVAR_ARCHIVE};
static cvar_t	r_smoothseams = {"r_smoothseams", "1", CVAR_ARCHIVE};

/* mdl_smooth.cs's settings */
#define SOLVE_RHO		1.0
#define SOLVE_RELAX		1.6
#define SOLVE_EPS		1e-2	/* grid steps: 1e-3 in mdl_smooth.cs; the same numbers, half the iterations (8.2), 6x below the packing */
#define SOLVE_MAX_ITERS		2000
#define SOLVE_H			0.49	/* < 0.5: rounding gives back the bytes */
#define LOOP_RATIO		1.5
#define MIN_FACE		0.25	/* grid cells: a smaller face counts by its area */
#define CARD_DOT		-0.5	/* table normals pointing against each other: a card's two sides */

typedef struct
{
	int		start, count;
	qboolean	loop, group;
} sequence_t;

/* a model's vertices, sequences and solved positions */
typedef struct smoothed_s
{
	struct smoothed_s	*next;
	char		name[MAX_QPATH];
	uint64_t	key;		/* the pose data's hash */
	int		num_verts, num_poses;
	float		*z;		/* [pose][vertex][axis], grid steps */
	int		num_seqs, loops;
	double		ms;		/* the solve */
	long		series, iterations;
	int		max_iterations, not_converged;
} smoothed_t;

static smoothed_t	*smoothed;	/* the session's */
static double		build_ms;	/* VK_ModelPoses' time this session */
static int		builds;

/* a model being built: its vertices (from the command vertices) */
typedef struct
{
	const aliashdr_t	*hdr;
	const trivertx_t	*pv;		/* hdr's poses: [pose][command vertex] */
	int		poseverts;	/* command vertices per pose */
	int		num_verts, num_poses, num_tris;
	int		*first;		/* [vertex]: its first command vertex */
	int		*tris;		/* [triangle][3]: vertices, in the triangles' winding */
	sequence_t	*seqs;
	int		num_seqs;
	int		*rep;		/* [vertex]: the vertex it welds to */
	byte		*keep;		/* [triangle]: counts in the normals, unwelded */
	byte		*keep_weld;	/* welded */
	qboolean	flip;		/* the triangles wind against the table normals */
	float		scale[3];
} model_t;

static uint64_t Fnv (uint64_t h, const void *data, size_t size)
{
	const byte	*p = (const byte *) data;

	while (size--)
		h = (h ^ *p++) * 1099511628211ull;
	return h;
}

/* seconds, from the performance counter (also on the worker threads) */
static double Now (void)
{
	LARGE_INTEGER	c, f;

	QueryPerformanceCounter (&c);
	QueryPerformanceFrequency (&f);
	return (double)c.QuadPart / (double)f.QuadPart;
}

static const trivertx_t *Vert (const model_t *m, int pose, int v)
{
	return &m->pv[pose * m->poseverts + m->first[v]];
}

static const float *TableNormal (const model_t *m, int pose, int v)
{
	return vertex_normals[q_min (Vert (m, pose, v)->lightnormalindex, NUM_VERTEX_NORMALS - 1)];
}


/* ==========================================================================
 * The model's vertices, triangles and sequences
 * ========================================================================== */

typedef struct
{
	uint64_t	hash;
	int		cmd;
} vertkey_t;

static int CompareKeys (const void *a, const void *b)
{
	const vertkey_t	*x = (const vertkey_t *) a, *y = (const vertkey_t *) b;

	if (x->hash != y->hash)
		return (x->hash < y->hash) ? -1 : 1;
	return x->cmd - y->cmd;
}

/* command vertices with the same bytes and table normal in every pose are
 * one vertex; remap[command vertex] = its vertex */
static void GroupVertices (model_t *m, uint16_t *remap)
{
	const aliashdr_t	*hdr = m->hdr;
	int		n = hdr->poseverts, i, j, pose;
	vertkey_t	*keys = (vertkey_t *) malloc (n * sizeof(vertkey_t));
	int		*rep = (int *) malloc (n * sizeof(int));

	if (!keys || !rep)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < n; i++)
	{
		uint64_t	h = 14695981039346656037ull;

		for (pose = 0; pose < hdr->numposes; pose++)
			h = Fnv (h, &m->pv[pose * n + i], sizeof(trivertx_t));
		keys[i].hash = h;
		keys[i].cmd = i;
	}
	qsort (keys, n, sizeof(vertkey_t), CompareKeys);
	/* the same hash and the same data: the first of them */
	for (i = 0; i < n; i++)
	{
		rep[keys[i].cmd] = keys[i].cmd;
		for (j = i - 1; j >= 0 && keys[j].hash == keys[i].hash; j--)
		{
			int	a = keys[j].cmd, b = keys[i].cmd;

			for (pose = 0; pose < hdr->numposes; pose++)
			{
				if (memcmp (&m->pv[pose * n + a], &m->pv[pose * n + b], sizeof(trivertx_t)))
					break;
			}
			if (pose == hdr->numposes)
			{
				rep[b] = rep[a];
				break;
			}
		}
	}
	/* numbered in the command vertices' order */
	m->first = (int *) malloc (n * sizeof(int));
	if (!m->first)
		Sys_Error ("%s: out of memory", __thisfunc__);
	m->num_verts = 0;
	for (i = 0; i < n; i++)
	{
		if (rep[i] == i)
		{
			m->first[m->num_verts] = i;
			remap[i] = (uint16_t) m->num_verts++;
		}
		else
			remap[i] = remap[rep[i]];
	}
	free (keys);
	free (rep);
}

static void StripStem (const char *name, char *out)
{
	int	len;

	q_strlcpy (out, name, 16);
	q_strlwr (out);
	for (len = (int)strlen (out); len > 0 && out[len - 1] >= '0' && out[len - 1] <= '9'; len--)
		;
	out[len] = 0;
}

static double PoseDistance (const model_t *m, int a, int b)
{
	double	sum = 0;
	int	v, k;

	for (v = 0; v < m->num_verts; v++)
	{
		double	d2 = 0;

		for (k = 0; k < 3; k++)
		{
			double	d = ((int)Vert (m, a, v)->v[k] - (int)Vert (m, b, v)->v[k]) * (double)m->scale[k];

			d2 += d * d;
		}
		sum += sqrt (d2);
	}
	return sum / q_max (m->num_verts, 1);
}

/* a frame group's subframes (a loop), or runs of single frames named
 * alike but for a trailing number; a loop when the last pose is about as
 * close to the first as the poses are to each other */
static void FindSequences (model_t *m)
{
	const aliashdr_t	*hdr = m->hdr;
	char		stem[16], next[16];
	int		f, g;

	m->seqs = (sequence_t *) calloc (q_max (hdr->numframes, 1), sizeof(sequence_t));
	if (!m->seqs)
		Sys_Error ("%s: out of memory", __thisfunc__);
	m->num_seqs = 0;
	for (f = 0; f < hdr->numframes; f = g)
	{
		sequence_t	*s = &m->seqs[m->num_seqs++];

		s->start = hdr->frames[f].firstpose;
		g = f + 1;
		if (hdr->frames[f].numposes > 1)
		{
			s->count = hdr->frames[f].numposes;
			s->group = s->loop = true;
		}
		else
		{
			StripStem (hdr->frames[f].name, stem);
			while (g < hdr->numframes && hdr->frames[g].numposes == 1 &&
			       hdr->frames[g].firstpose == hdr->frames[g - 1].firstpose + 1)
			{
				StripStem (hdr->frames[g].name, next);
				if (strcmp (next, stem))
					break;
				g++;
			}
			s->count = hdr->frames[g - 1].firstpose - s->start + 1;
		}
		if (s->count >= 2 && !s->group)
		{
			double	step = 0, wrap;
			int	t;

			for (t = 0; t + 1 < s->count; t++)
				step += PoseDistance (m, s->start + t, s->start + t + 1);
			step /= s->count - 1;
			wrap = PoseDistance (m, s->start + s->count - 1, s->start);
			s->loop = wrap <= LOOP_RATIO * step;
		}
	}
}

/* the triangles over the vertices, in the shader's winding */
static void ModelTriangles (model_t *m, const AliasTriangle *tris, int num_tris, const uint16_t *remap)
{
	int	t;

	m->num_tris = num_tris;
	m->tris = (int *) malloc (q_max (num_tris, 1) * 3 * sizeof(int));
	if (!m->tris)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (t = 0; t < num_tris; t++)
	{
		m->tris[t * 3 + 0] = remap[tris[t].verts[0] & 0xffff];
		m->tris[t * 3 + 1] = remap[tris[t].verts[0] >> 16];
		m->tris[t * 3 + 2] = remap[tris[t].verts[1]];
	}
}


/* ==========================================================================
 * The solver (mdl_smooth.cs's ADMM)
 * ========================================================================== */

/* DtD + rho I = L Lt, D the second differences (cyclic for a loop); L
 * row-major with each row's first nonzero column (Cholesky keeps the
 * envelope: the band, and for a loop the last two rows) */
typedef struct
{
	int	n;
	double	*L;
	double	*inv;		/* 1 / the diagonal */
	int	*first;
} factor_t;

/* false: out of memory (nothing kept) */
static qboolean Factor (factor_t *f, int n, qboolean loop)
{
	double	*a = (double *) calloc ((size_t)n * n, sizeof(double));
	int	rows = loop ? n : n - 2, r, i, j, k;
	static const double	w[3] = { 1, -2, 1 };

	f->n = n;
	f->L = (double *) calloc ((size_t)n * n, sizeof(double));
	f->inv = (double *) malloc (n * sizeof(double));
	f->first = (int *) malloc (n * sizeof(int));
	if (!a || !f->L || !f->inv || !f->first)
	{
		free (a);
		free (f->L);
		free (f->inv);
		free (f->first);
		return false;
	}
	for (r = 0; r < rows; r++)
	{
		int	idx[3];

		if (loop)
		{
			idx[0] = (r + n - 1) % n;	idx[1] = r;	idx[2] = (r + 1) % n;
		}
		else
		{
			idx[0] = r;	idx[1] = r + 1;	idx[2] = r + 2;
		}
		for (i = 0; i < 3; i++)
		{
			for (j = 0; j < 3; j++)
				a[idx[i] * n + idx[j]] += w[i] * w[j];
		}
	}
	for (i = 0; i < n; i++)
		a[i * n + i] += SOLVE_RHO;
	for (i = 0; i < n; i++)
	{
		int	first = 0;

		while (a[i * n + first] == 0)
			first++;
		f->first[i] = first;
		for (j = first; j <= i; j++)
		{
			double	s = a[i * n + j];

			for (k = q_max (first, f->first[j]); k < j; k++)
				s -= f->L[i * n + k] * f->L[j * n + k];
			f->L[i * n + j] = (i == j) ? sqrt (s) : s / f->L[j * n + j];
		}
		f->inv[i] = 1.0 / f->L[i * n + i];
	}
	free (a);
	return true;
}

/* X = (L Lt)^-1 Y for the S columns of Y ([row][S]); Y is overwritten:
 * every series of a sequence at once, so the inner loops run along the
 * series */
static void SolveColumns (const factor_t *f, double *y, double *x, int S, int stride)
{
	const double	*L = f->L;
	int		n = f->n, i, k, s;

	for (i = 0; i < n; i++)
	{
		double	*yi = y + i * stride;

		for (k = f->first[i]; k < i; k++)
		{
			const double	l = L[i * n + k], *yk = y + k * stride;

			for (s = 0; s < S; s++)
				yi[s] -= l * yk[s];
		}
		for (s = 0; s < S; s++)
			yi[s] *= f->inv[i];
	}
	for (i = n - 1; i >= 0; i--)
	{
		double	*xi = x + i * stride, *yi = y + i * stride;

		for (s = 0; s < S; s++)
			xi[s] = yi[s] * f->inv[i];
		for (k = f->first[i]; k < i; k++)
		{
			const double	l = L[i * n + k];
			double		*yk = y + k * stride;

			for (s = 0; s < S; s++)
				yk[s] -= l * xi[s];
		}
	}
}

/* see the top; false: out of memory (the worker threads mustn't call Sys_Error).
 * Per sequence, the series (a vertex's path on an axis) that
 * move, all at once, a column each; a converged series leaves (its last
 * column moves into its place) */
static qboolean SmoothPositions (model_t *m, smoothed_t *s)
{
	double	start = Now ();
	int	nv = m->num_verts, i, v, a;

	for (i = 0; i < m->num_poses * nv; i++)
	{
		for (a = 0; a < 3; a++)
			s->z[i * 3 + a] = Vert (m, i / nv, i % nv)->v[a];
	}
	s->num_seqs = m->num_seqs;
	for (i = 0; i < m->num_seqs; i++)
	{
		const sequence_t	*sq = &m->seqs[i];
		int			n = sq->count, S = 0, stride = 0, t, c, k;
		factor_t		fac;
		double			*q, *x, *z, *u, *y;
		int			*series, *iters;

		if (sq->loop)
			s->loops++;
		if (n < 3)
			continue;
		for (v = 0; v < nv; v++)	/* the moving series: a column each */
		{
			for (a = 0; a < 3; a++)
			{
				int	first = Vert (m, sq->start, v)->v[a];

				for (t = 1; t < n && Vert (m, sq->start + t, v)->v[a] == first; t++)
					;
				if (t < n)
					stride++;
			}
		}
		if (!stride)
			continue;
		if (!Factor (&fac, n, sq->loop))
			return false;
		q = (double *) malloc ((size_t)5 * n * stride * sizeof(double));
		series = (int *) malloc (stride * sizeof(int));
		iters = (int *) calloc (stride, sizeof(int));
		if (!q || !series || !iters)
		{
			free (q);
			free (series);
			free (iters);
			free (fac.L);
			free (fac.inv);
			free (fac.first);
			return false;
		}
		x = q + n * stride;
		z = x + n * stride;
		u = z + n * stride;
		y = u + n * stride;

		/* a constant series is already the smoothest */
		for (v = 0; v < nv; v++)
		{
			for (a = 0; a < 3; a++)
			{
				int	first = Vert (m, sq->start, v)->v[a];

				for (t = 1; t < n && Vert (m, sq->start + t, v)->v[a] == first; t++)
					;
				if (t == n)
					continue;
				series[S] = v * 3 + a;
				for (t = 0; t < n; t++)
				{
					q[t * stride + S] = z[t * stride + S] = Vert (m, sq->start + t, v)->v[a];
					u[t * stride + S] = 0;
				}
				S++;
			}
		}
		s->series += S;

		for (k = 0; S > 0 && k < SOLVE_MAX_ITERS; k++)
		{
			for (t = 0; t < n; t++)
			{
				const double	*zt = z + t * stride, *ut = u + t * stride;
				double		*yt = y + t * stride;

				for (c = 0; c < S; c++)
					yt[c] = SOLVE_RHO * (zt[c] - ut[c]);
			}
			SolveColumns (&fac, y, x, S, stride);
			/* y: each series' residuals, r in row 0 and the change of z in row 1 */
			for (c = 0; c < S; c++)
				y[c] = y[stride + c] = 0;
			for (t = 0; t < n; t++)
			{
				const double	*qt = q + t * stride, *xt = x + t * stride;
				double		*zt = z + t * stride, *ut = u + t * stride;

				for (c = 0; c < S; c++)
				{
					double	xr = SOLVE_RELAX * xt[c] + (1 - SOLVE_RELAX) * zt[c];
					double	zn = q_min (q_max (xr + ut[c], qt[c] - SOLVE_H), qt[c] + SOLVE_H);

					ut[c] += xr - zn;
					y[c] = q_max (y[c], fabs (xt[c] - zn));
					y[stride + c] = q_max (y[stride + c], fabs (zn - zt[c]));
					zt[c] = zn;
				}
			}
			for (c = 0; c < S; c++)
			{
				iters[c]++;
				if (y[c] < SOLVE_EPS && SOLVE_RHO * y[stride + c] < SOLVE_EPS)
				{
					/* done: out, and the last column in its place */
					for (t = 0; t < n; t++)
						s->z[((sq->start + t) * nv + series[c] / 3) * 3 + series[c] % 3] = (float) z[t * stride + c];
					s->iterations += iters[c];
					s->max_iterations = q_max (s->max_iterations, iters[c]);
					S--;
					if (c < S)
					{
						for (t = 0; t < n; t++)
						{
							q[t * stride + c] = q[t * stride + S];
							z[t * stride + c] = z[t * stride + S];
							u[t * stride + c] = u[t * stride + S];
						}
						y[c] = y[S];
						y[stride + c] = y[stride + S];
						series[c] = series[S];
						iters[c] = iters[S];
						c--;	/* the moved one is looked at next */
					}
				}
			}
		}
		for (c = 0; c < S; c++)	/* not converged */
		{
			for (t = 0; t < n; t++)
				s->z[((sq->start + t) * nv + series[c] / 3) * 3 + series[c] % 3] = (float) z[t * stride + c];
			s->iterations += iters[c];
			s->max_iterations = q_max (s->max_iterations, iters[c]);
			s->not_converged++;
		}
		free (q);
		free (series);
		free (iters);
		free (fac.L);
		free (fac.inv);
		free (fac.first);
	}
	s->ms = (Now () - start) * 1000.0;
	return true;
}



/* ==========================================================================
 * Normals
 * ========================================================================== */

/* a pose's position of a vertex in model units: smoothed (z) or the bytes */
static void Position (const model_t *m, const float *z, int pose, int v, double out[3])
{
	int	a;

	for (a = 0; a < 3; a++)
	{
		double	g = z ? z[(pose * m->num_verts + v) * 3 + a] : Vert (m, pose, v)->v[a];

		out[a] = g * m->scale[a] + m->hdr->scale_origin[a];
	}
}

static void FaceNormal (const model_t *m, const float *z, int pose, int t, double c[3])
{
	double	p[3][3], e1[3], e2[3];
	int	k;

	for (k = 0; k < 3; k++)
		Position (m, z, pose, m->tris[t * 3 + k], p[k]);
	for (k = 0; k < 3; k++)
	{
		e1[k] = p[1][k] - p[0][k];
		e2[k] = p[2][k] - p[0][k];
	}
	c[0] = e1[1] * e2[2] - e1[2] * e2[1];
	c[1] = e1[2] * e2[0] - e1[0] * e2[2];
	c[2] = e1[0] * e2[1] - e1[1] * e2[0];
	if (m->flip)
	{
		c[0] = -c[0];	c[1] = -c[1];	c[2] = -c[2];
	}
}

/* rebuilt normals of one pose (n[vertex][3]): see the top. Without filter,
 * every face's unit normal (the winding test) */
static void Rebuild (const model_t *m, const float *z, int pose, qboolean weld, qboolean filter, double *n, double *weight)
{
	const byte	*keep = weld ? m->keep_weld : m->keep;
	double		s = q_max (m->scale[0], q_max (m->scale[1], m->scale[2]));
	double		full = 2 * MIN_FACE * s * s;	/* twice the area of a face that counts fully */
	int		t, k, v, a;

	memset (n, 0, m->num_verts * 3 * sizeof(double));
	memset (weight, 0, m->num_verts * sizeof(double));
	for (t = 0; t < m->num_tris; t++)
	{
		double	c[3], l, w;

		if (filter && keep && !keep[t])
			continue;
		FaceNormal (m, z, pose, t, c);
		l = sqrt (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (l == 0)
			continue;
		w = filter ? q_min (1.0, l / full) : 1.0;
		for (k = 0; k < 3; k++)
		{
			v = m->tris[t * 3 + k];
			if (filter && weld)
				v = m->rep[v];
			for (a = 0; a < 3; a++)
				n[v * 3 + a] += c[a] * w / l;
			weight[v] += w;
		}
	}
	for (v = 0; v < m->num_verts; v++)
	{
		const float	*tn = TableNormal (m, pose, v);
		double		l;

		if (filter && weld && m->rep[v] != v)
			continue;	/* copied from its representative below */
		if (filter && weight[v] < 1)	/* less than a full face: the table fills in */
		{
			for (a = 0; a < 3; a++)
				n[v * 3 + a] += (1 - weight[v]) * tn[a];
		}
		l = sqrt (n[v * 3] * n[v * 3] + n[v * 3 + 1] * n[v * 3 + 1] + n[v * 3 + 2] * n[v * 3 + 2]);
		for (a = 0; a < 3; a++)
			n[v * 3 + a] = (l > 0) ? n[v * 3 + a] / l : tn[a];
	}
	if (filter && weld)
	{
		for (v = 0; v < m->num_verts; v++)
		{
			for (a = 0; a < 3; a++)
				n[v * 3 + a] = n[m->rep[v] * 3 + a];
		}
	}
}

static double TableAgreement (const model_t *m, int a, int b)
{
	double	d = 0;
	int	pose;

	for (pose = 0; pose < m->num_poses; pose++)
		d += DotProduct (TableNormal (m, pose, a), TableNormal (m, pose, b));
	return d / q_max (m->num_poses, 1);
}

static int CompareTriples (const void *a, const void *b)
{
	const int	*x = (const int *) a, *y = (const int *) b;
	int		k;

	for (k = 0; k < 3; k++)
	{
		if (x[k] != y[k])
			return x[k] - y[k];
	}
	return x[3] - y[3];	/* the triangle */
}

static void SortedTriple (const model_t *m, int t, const int *map, int out[4])
{
	int	k, tmp;

	for (k = 0; k < 3; k++)
		out[k] = map ? map[m->tris[t * 3 + k]] : m->tris[t * 3 + k];
	if (out[0] > out[1]) { tmp = out[0]; out[0] = out[1]; out[1] = tmp; }
	if (out[1] > out[2]) { tmp = out[1]; out[1] = out[2]; out[2] = tmp; }
	if (out[0] > out[1]) { tmp = out[0]; out[0] = out[1]; out[1] = tmp; }
	out[3] = t;
}

/* of faces on the same three vertices (map: welded, or NULL) the one
 * that agrees best with the table's normals at them over all poses */
static void KeepFaces (const model_t *m, const int *map, byte *keep)
{
	int	*sorted = (int *) malloc (q_max (m->num_tris, 1) * 4 * sizeof(int));
	int	i, j, t, pose, k;

	if (!sorted)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (t = 0; t < m->num_tris; t++)
	{
		keep[t] = 1;
		SortedTriple (m, t, map, &sorted[t * 4]);
	}
	qsort (sorted, m->num_tris, 4 * sizeof(int), CompareTriples);
	for (i = 0; i < m->num_tris; i = j)
	{
		int	best = sorted[i * 4 + 3];
		double	best_score = -1e30;

		for (j = i; j < m->num_tris && !memcmp (&sorted[j * 4], &sorted[i * 4], 3 * sizeof(int)); j++)
			;
		if (j - i < 2)
			continue;
		for (k = i; k < j; k++)
		{
			double	score = 0, c[3];
			int	c3;

			t = sorted[k * 4 + 3];
			for (pose = 0; pose < m->num_poses; pose++)
			{
				FaceNormal (m, NULL, pose, t, c);
				for (c3 = 0; c3 < 3; c3++)
				{
					const float	*tn = TableNormal (m, pose, m->tris[t * 3 + c3]);

					score += c[0] * tn[0] + c[1] * tn[1] + c[2] * tn[2];
				}
			}
			if (score > best_score)
			{
				best_score = score;
				best = t;
			}
		}
		for (k = i; k < j; k++)
			keep[sorted[k * 4 + 3]] = (sorted[k * 4 + 3] == best);
	}
	free (sorted);
}

/* the winding, the welded vertices and the back-to-back faces: decided
 * once per model, so nothing changes between frames */
static void PrepareNormals (model_t *m)
{
	double		*n = (double *) malloc (m->num_verts * 3 * sizeof(double));
	double		*w = (double *) malloc (m->num_verts * sizeof(double));
	int		*sorted, *cards, i, j, k, v;
	double		d = 0;

	m->rep = (int *) malloc (m->num_verts * sizeof(int));
	m->keep = (byte *) malloc (q_max (m->num_tris, 1));
	m->keep_weld = (byte *) malloc (q_max (m->num_tris, 1));
	sorted = (int *) malloc (q_max (m->num_tris, 1) * 4 * sizeof(int));
	cards = (int *) calloc (m->num_verts, sizeof(int));
	if (!n || !w || !m->rep || !m->keep || !m->keep_weld || !sorted || !cards)
		Sys_Error ("%s: out of memory", __thisfunc__);

	/* the winding against the table at the first pose */
	m->flip = false;
	Rebuild (m, NULL, 0, false, false, n, w);
	for (v = 0; v < m->num_verts; v++)
		d += DotProduct (TableNormal (m, 0, v), &n[v * 3]);	/* float . double */
	m->flip = d < 0;

	/* welded: the first vertex at the same place in every pose, not a
	 * side facing away from it */
	for (v = 0; v < m->num_verts; v++)
	{
		m->rep[v] = v;
		for (i = 0; i < v; i++)
		{
			int	pose;

			if (m->rep[i] != i)
				continue;
			for (pose = 0; pose < m->num_poses; pose++)
			{
				if (memcmp (Vert (m, pose, i)->v, Vert (m, pose, v)->v, 3))
					break;
			}
			if (pose == m->num_poses)
			{
				if (TableAgreement (m, v, i) >= CARD_DOT)
					m->rep[v] = i;
				break;
			}
		}
	}
	/* not a card's two sides: faces that lie on each other only once
	 * welded keep their vertices apart */
	for (i = 0; i < m->num_tris; i++)
		SortedTriple (m, i, m->rep, &sorted[i * 4]);
	qsort (sorted, m->num_tris, 4 * sizeof(int), CompareTriples);
	for (i = 0; i < m->num_tris; i = j)
	{
		int	raw0[4], raw[4];
		qboolean	distinct = false;

		for (j = i; j < m->num_tris && !memcmp (&sorted[j * 4], &sorted[i * 4], 3 * sizeof(int)); j++)
			;
		SortedTriple (m, sorted[i * 4 + 3], NULL, raw0);
		for (k = i + 1; k < j; k++)
		{
			SortedTriple (m, sorted[k * 4 + 3], NULL, raw);
			if (memcmp (raw, raw0, 3 * sizeof(int)))
				distinct = true;
		}
		if (distinct)
		{
			for (k = i; k < j; k++)
			{
				int	c3;

				for (c3 = 0; c3 < 3; c3++)
					cards[m->rep[m->tris[sorted[k * 4 + 3] * 3 + c3]]] = 1;
			}
		}
	}
	for (v = 0; v < m->num_verts; v++)	/* whole classes: cards[] is by representative */
	{
		if (cards[m->rep[v]])
			m->rep[v] = v;
	}
	KeepFaces (m, NULL, m->keep);
	KeepFaces (m, m->rep, m->keep_weld);

	free (n);
	free (w);
	free (sorted);
	free (cards);
}

/* the mean angle (degrees) between a frame's normal and the direction
 * halfway between its neighbours', over the sequences of 3 poses or more;
 * normals[pose] = n[vertex][3] */
static double NormalJerk (const model_t *m, double **normals, double *over5)
{
	double	sum = 0;
	long	count = 0, over = 0;
	int	i, r, v;

	for (i = 0; i < m->num_seqs; i++)
	{
		const sequence_t	*sq = &m->seqs[i];
		int			n = sq->count, rows = sq->loop ? n : n - 2;

		if (n < 3)
			continue;
		for (r = 0; r < rows; r++)
		{
			int	t0 = sq->loop ? (r + n - 1) % n : r, t1 = sq->loop ? r : r + 1, t2 = sq->loop ? (r + 1) % n : r + 2;
			double	*a = normals[sq->start + t0], *b = normals[sq->start + t1], *c = normals[sq->start + t2];

			for (v = 0; v < m->num_verts; v++)
			{
				double	h[3], l, dot, ang;
				int	k;

				for (k = 0; k < 3; k++)
					h[k] = a[v * 3 + k] + c[v * 3 + k];
				l = sqrt (h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
				if (l < 1e-9)
					ang = 0;
				else
				{
					dot = (b[v * 3] * h[0] + b[v * 3 + 1] * h[1] + b[v * 3 + 2] * h[2]) / l;
					ang = acos (q_max (-1.0, q_min (1.0, dot))) * (180.0 / M_PI);
				}
				sum += ang;
				count++;
				if (ang > 5)
					over++;
			}
		}
	}
	if (over5)
		*over5 = count ? 100.0 * over / count : 0;
	return count ? sum / count : 0;
}

/* every pose's normals: the table's, or rebuilt (z: smoothed or NULL) */
static double **AllNormals (const model_t *m, const float *z, qboolean table, qboolean weld)
{
	double	**normals = (double **) malloc (m->num_poses * sizeof(double *));
	double	*w = (double *) malloc (m->num_verts * sizeof(double));
	int	pose, v, a;

	if (!normals || !w)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (pose = 0; pose < m->num_poses; pose++)
	{
		normals[pose] = (double *) malloc (m->num_verts * 3 * sizeof(double));
		if (!normals[pose])
			Sys_Error ("%s: out of memory", __thisfunc__);
		if (table)
		{
			for (v = 0; v < m->num_verts; v++)
			{
				for (a = 0; a < 3; a++)
					normals[pose][v * 3 + a] = TableNormal (m, pose, v)[a];
			}
		}
		else
			Rebuild (m, z, pose, weld, true, normals[pose], w);
	}
	free (w);
	return normals;
}

static void FreeNormals (const model_t *m, double **normals)
{
	int	pose;

	for (pose = 0; pose < m->num_poses; pose++)
		free (normals[pose]);
	free (normals);
}


/* ==========================================================================
 * Packing
 * ========================================================================== */

/* model_geometry.comp's decode_oct14 */
static uint32_t EncodeOct14 (const double n[3])
{
	double	l1 = fabs (n[0]) + fabs (n[1]) + fabs (n[2]);
	double	x, y;
	int	ux, uy;

	if (l1 <= 0)
		return (8192u) | (8192u << 14);
	x = n[0] / l1;
	y = n[1] / l1;
	if (n[2] < 0)
	{
		double	ox = (1 - fabs (y)) * (x >= 0 ? 1 : -1);
		double	oy = (1 - fabs (x)) * (y >= 0 ? 1 : -1);

		x = ox;
		y = oy;
	}
	ux = (int) floor ((x * 0.5 + 0.5) * 16383.0 + 0.5);
	uy = (int) floor ((y * 0.5 + 0.5) * 16383.0 + 0.5);
	ux = q_max (0, q_min (16383, ux));
	uy = q_max (0, q_min (16383, uy));
	return (uint32_t) ux | ((uint32_t) uy << 14);
}

void VK_DecodePoseVertex (const AliasModel *am, const uint32_t v[2], vec3_t pos, vec3_t nrm)
{
	uint32_t	q[3], e = v[1] >> 4;
	float		t;
	int		a;

	q[0] = v[0] & 0xfff;
	q[1] = (v[0] >> 12) & 0xfff;
	q[2] = (v[0] >> 24) | ((v[1] & 0xf) << 8);
	for (a = 0; a < 3; a++)
		pos[a] = ((float)q[a] * (1.0f / 16.0f) - 0.5f) * am->scale[a] + am->scale_origin[a];
	nrm[0] = (float)(e & 0x3fff) / 16383.0f * 2.0f - 1.0f;
	nrm[1] = (float)((e >> 14) & 0x3fff) / 16383.0f * 2.0f - 1.0f;
	nrm[2] = 1.0f - fabsf (nrm[0]) - fabsf (nrm[1]);
	t = q_max (0.0f, -nrm[2]);
	nrm[0] += (nrm[0] >= 0.0f) ? -t : t;
	nrm[1] += (nrm[1] >= 0.0f) ? -t : t;
	VectorNormalize (nrm);
}

static void Pack (double g[3], const double n[3], uint32_t out[2])
{
	uint32_t	q[3], e = EncodeOct14 (n);
	int		a;

	for (a = 0; a < 3; a++)
		q[a] = (uint32_t) q_max (0, q_min (4095, (int) floor ((g[a] + 0.5) * 16.0 + 0.5)));
	out[0] = q[0] | (q[1] << 12) | ((q[2] & 0xff) << 24);
	out[1] = (q[2] >> 8) | (e << 4);
}


/* ==========================================================================
 * Building a model's poses
 * ========================================================================== */

static uint64_t PoseKey (const aliashdr_t *hdr, const trivertx_t *pv)
{
	uint64_t	h = 14695981039346656037ull;

	h = Fnv (h, &hdr->numposes, sizeof(hdr->numposes));
	h = Fnv (h, &hdr->poseverts, sizeof(hdr->poseverts));
	return Fnv (h, pv, (size_t)hdr->numposes * hdr->poseverts * sizeof(trivertx_t));
}

static smoothed_t *FindSmoothed (const qmodel_t *model, uint64_t key)
{
	smoothed_t	*s;

	for (s = smoothed; s; s = s->next)
	{
		if (s->key == key && !strcmp (s->name, model->name))
			return s;
	}
	return NULL;
}

static void InitModel (model_t *m, const aliashdr_t *hdr, const AliasTriangle *tris, int num_tris, uint16_t *remap)
{
	memset (m, 0, sizeof(*m));
	m->hdr = hdr;
	m->pv = (const trivertx_t *)((const byte *)hdr + hdr->posedata);
	m->poseverts = hdr->poseverts;
	m->num_poses = hdr->numposes;
	VectorCopy (hdr->scale, m->scale);
	GroupVertices (m, remap);
	ModelTriangles (m, tris, num_tris, remap);
	FindSequences (m);
	PrepareNormals (m);
}

static void FreeModel (model_t *m)
{
	free (m->first);
	free (m->tris);
	free (m->seqs);
	free (m->rep);
	free (m->keep);
	free (m->keep_weld);
}

static smoothed_t *NewSmoothed (const qmodel_t *model, uint64_t key, const model_t *m)
{
	smoothed_t	*s = (smoothed_t *) calloc (1, sizeof(smoothed_t));

	if (s)
		s->z = (float *) malloc ((size_t)m->num_poses * m->num_verts * 3 * sizeof(float));
	if (!s || !s->z)
		Sys_Error ("%s: out of memory", __thisfunc__);
	q_strlcpy (s->name, model->name, sizeof(s->name));
	s->key = key;
	s->num_verts = m->num_verts;
	s->num_poses = m->num_poses;
	return s;
}

/* the model's solved positions, from the session's or solved now */
static smoothed_t *Smoothed (const qmodel_t *model, model_t *m, qboolean *solved)
{
	uint64_t	key = PoseKey (m->hdr, m->pv);
	smoothed_t	*s = FindSmoothed (model, key);

	*solved = !s;
	if (s)
		return s;
	s = NewSmoothed (model, key, m);
	if (!SmoothPositions (m, s))
		Sys_Error ("%s: out of memory", __thisfunc__);
	s->next = smoothed;
	smoothed = s;
	return s;
}

/* at map load, the precached models' solves on worker threads: each job
 * its own copy of the pose data (the model cache can move while the main
 * thread loads models) and its vertices and sequences, found on the main
 * thread; the workers only solve (no engine calls). The builds then find
 * them solved */
#define MAX_SOLVE_THREADS	15	/* with the main thread, 16 */

typedef struct
{
	model_t		m;
	trivertx_t	*pv;
	smoothed_t	*s;
	double		cost;
} job_t;

static job_t		*jobs;
static int		num_jobs;
static volatile LONG	next_job;
static volatile LONG	jobs_failed;	/* out of memory on a thread: Sys_Error after the join */
static struct
{
	int	models, threads;
	double	ms;		/* wall time */
} last_presolve;

static DWORD WINAPI SolveWorker (LPVOID arg)
{
	LONG	j;

	(void)arg;
	while ((j = InterlockedIncrement (&next_job) - 1) < num_jobs)
	{
		if (!SmoothPositions (&jobs[j].m, jobs[j].s))
			InterlockedExchange (&jobs_failed, 1);
	}
	return 0;
}

static int CompareJobs (const void *a, const void *b)
{
	double	x = ((const job_t *) a)->cost, y = ((const job_t *) b)->cost;

	return (x > y) ? -1 : (x < y) ? 1 : 0;	/* the largest first */
}

void VK_PresolveModels (qmodel_t **models, int count)
{
	double		start = Now ();
	HANDLE		handles[MAX_SOLVE_THREADS];
	SYSTEM_INFO	si;
	int		i, k, threads;

	if (!r_smoothmodels.integer || count <= 0)
		return;
	jobs = (job_t *) calloc (count, sizeof(job_t));
	if (!jobs)
		Sys_Error ("%s: out of memory", __thisfunc__);
	num_jobs = 0;
	for (i = 0; i < count; i++)
	{
		const aliashdr_t	*hdr;
		const trivertx_t	*pv;
		job_t			*j = &jobs[num_jobs];
		uint16_t		*remap;
		size_t			size;
		uint64_t		key;

		if (!models[i] || models[i]->type != mod_alias)
			continue;
		hdr = (const aliashdr_t *) Mod_Extradata (models[i]);
		if (hdr->numposes <= 0 || hdr->poseverts <= 0 || hdr->poseverts > 0xffff)
			continue;
		pv = (const trivertx_t *)((const byte *)hdr + hdr->posedata);
		key = PoseKey (hdr, pv);
		if (FindSmoothed (models[i], key))
			continue;
		for (k = 0; k < num_jobs; k++)
		{
			if (jobs[k].s->key == key && !strcmp (jobs[k].s->name, models[i]->name))
				break;
		}
		if (k < num_jobs)
			continue;	/* precached twice */

		size = (size_t)hdr->numposes * hdr->poseverts * sizeof(trivertx_t);
		j->pv = (trivertx_t *) malloc (size);
		remap = (uint16_t *) malloc (hdr->poseverts * sizeof(uint16_t));
		if (!j->pv || !remap)
			Sys_Error ("%s: out of memory", __thisfunc__);
		memcpy (j->pv, pv, size);
		j->m.hdr = hdr;
		j->m.pv = j->pv;
		j->m.poseverts = hdr->poseverts;
		j->m.num_poses = hdr->numposes;
		VectorCopy (hdr->scale, j->m.scale);
		GroupVertices (&j->m, remap);
		FindSequences (&j->m);
		free (remap);
		j->m.hdr = NULL;	/* the workers don't look at it */
		j->s = NewSmoothed (models[i], key, &j->m);
		for (k = 0; k < j->m.num_seqs; k++)
		{
			if (j->m.seqs[k].count >= 3)
				j->cost += (double)j->m.seqs[k].count * j->m.num_verts;
		}
		num_jobs++;
	}

	if (num_jobs)
	{
		qsort (jobs, num_jobs, sizeof(job_t), CompareJobs);
		GetSystemInfo (&si);
		/* the workers and this thread: as many as the cores */
		threads = q_max (0, q_min ((int)si.dwNumberOfProcessors - 1, q_min (MAX_SOLVE_THREADS, num_jobs - 1)));
		next_job = 0;
		jobs_failed = 0;
		for (i = 0; i < threads; i++)
		{
			handles[i] = CreateThread (NULL, 0, SolveWorker, NULL, 0, NULL);
			if (!handles[i])
				break;
		}
		threads = i;
		SolveWorker (NULL);
		if (threads)
		{
			WaitForMultipleObjects (threads, handles, TRUE, INFINITE);
			for (i = 0; i < threads; i++)
				CloseHandle (handles[i]);
		}
		if (jobs_failed)
			Sys_Error ("%s: out of memory", __thisfunc__);
		for (i = 0; i < num_jobs; i++)
		{
			jobs[i].s->next = smoothed;
			smoothed = jobs[i].s;
			free (jobs[i].pv);
			FreeModel (&jobs[i].m);
		}
		last_presolve.models = num_jobs;
		last_presolve.threads = threads + 1;
		last_presolve.ms = (Now () - start) * 1000.0;
	}
	free (jobs);
	jobs = NULL;
	num_jobs = 0;
}

/* see the top; the caller frees out->remap and out->poses */
void VK_ModelPoses (const qmodel_t *model, const aliashdr_t *hdr, const AliasTriangle *tris, int num_tris, vk_modelposes_t *out)
{
	model_t		m;
	smoothed_t	*s = NULL;
	double		**normals, g[3];
	int		pose, v, a;

	double		start = Now ();

	memset (out, 0, sizeof(*out));
	out->remap = (uint16_t *) malloc (hdr->poseverts * sizeof(uint16_t));
	if (!out->remap)
		Sys_Error ("%s: out of memory", __thisfunc__);
	InitModel (&m, hdr, tris, num_tris, out->remap);
	out->num_verts = m.num_verts;

	if (r_smoothmodels.integer)
	{
		qboolean	weld = r_smoothseams.integer != 0;
		double		**table;

		qboolean	solved;

		s = Smoothed (model, &m, &solved);
		out->solve_ms = solved ? s->ms : 0;
		normals = AllNormals (&m, s->z, false, weld);
		/* a model whose rebuilt normals change more between frames than
		 * its table's keeps the table's (8.1: some effects) */
		table = AllNormals (&m, NULL, true, false);
		if (NormalJerk (&m, normals, NULL) > NormalJerk (&m, table, NULL))
		{
			FreeNormals (&m, normals);
			normals = table;
			out->table_normals = true;
		}
		else
			FreeNormals (&m, table);
	}
	else
	{
		normals = AllNormals (&m, NULL, true, false);
		out->table_normals = true;
	}

	out->poses = (uint32_t *) malloc ((size_t)m.num_poses * m.num_verts * 2 * sizeof(uint32_t));
	if (!out->poses)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (pose = 0; pose < m.num_poses; pose++)
	{
		for (v = 0; v < m.num_verts; v++)
		{
			for (a = 0; a < 3; a++)
				g[a] = s ? s->z[(pose * m.num_verts + v) * 3 + a] : Vert (&m, pose, v)->v[a];
			Pack (g, &normals[pose][v * 3], &out->poses[(pose * m.num_verts + v) * 2]);
		}
	}
	FreeNormals (&m, normals);
	FreeModel (&m);
	build_ms += (Now () - start) * 1000.0;
	builds++;
}


/* ==========================================================================
 * vk_models smooth <model>: mdl_smooth.ps1's numbers
 * ========================================================================== */

static int CompareDoubles (const void *a, const void *b)
{
	double	x = *(const double *) a, y = *(const double *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double Percentile (double *v, int n, double p)
{
	if (!n)
		return 0;
	qsort (v, n, sizeof(double), CompareDoubles);
	return v[q_min ((int)(n * p), n - 1)];
}

static int CompareEdges (const void *a, const void *b)
{
	const int	*x = (const int *) a, *y = (const int *) b;

	return (x[0] != y[0]) ? x[0] - y[0] : x[1] - y[1];
}

/* the second differences' RMS (grid steps) and the edges' length
 * variation against rounding alone (median, 90th percentile), before
 * (z NULL) or after */
static void ShapeStats (const model_t *m, const float *z, double *accel, double *edge_med, double *edge_p90)
{
	int	*edges = (int *) malloc (q_max (m->num_tris, 1) * 3 * 2 * sizeof(int));
	double	*ratios = NULL, acc = 0, maxs = q_max (m->scale[0], q_max (m->scale[1], m->scale[2]));
	long	acc_n = 0;
	int	num_edges = 0, num_ratios = 0, max_ratios = 0, i, k, e, t, v, a;

	if (!edges)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (t = 0; t < m->num_tris; t++)
	{
		for (k = 0; k < 3; k++)
		{
			int	p = m->tris[t * 3 + k], q = m->tris[t * 3 + (k + 1) % 3];

			edges[num_edges * 2] = q_min (p, q);
			edges[num_edges * 2 + 1] = q_max (p, q);
			num_edges++;
		}
	}
	qsort (edges, num_edges, 2 * sizeof(int), CompareEdges);
	for (i = 1, k = 1; i < num_edges; i++)
	{
		if (edges[i * 2] != edges[(k - 1) * 2] || edges[i * 2 + 1] != edges[(k - 1) * 2 + 1])
		{
			edges[k * 2] = edges[i * 2];
			edges[k * 2 + 1] = edges[i * 2 + 1];
			k++;
		}
	}
	num_edges = num_edges ? k : 0;

	for (i = 0; i < m->num_seqs; i++)
	{
		const sequence_t	*sq = &m->seqs[i];
		int			n = sq->count, rows = sq->loop ? n : n - 2, r;

		if (n < 3)
			continue;
		for (v = 0; v < m->num_verts; v++)
		{
			for (a = 0; a < 3; a++)
			{
				for (r = 0; r < rows; r++)
				{
					int	t0 = sq->loop ? (r + n - 1) % n : r, t1 = sq->loop ? r : r + 1, t2 = sq->loop ? (r + 1) % n : r + 2;
					double	g0, g1, g2, d;

					g0 = z ? z[((sq->start + t0) * m->num_verts + v) * 3 + a] : Vert (m, sq->start + t0, v)->v[a];
					g1 = z ? z[((sq->start + t1) * m->num_verts + v) * 3 + a] : Vert (m, sq->start + t1, v)->v[a];
					g2 = z ? z[((sq->start + t2) * m->num_verts + v) * 3 + a] : Vert (m, sq->start + t2, v)->v[a];
					d = g0 - 2 * g1 + g2;
					acc += d * d;
					acc_n++;
				}
			}
		}
		if (n < 4)
			continue;
		/* an edge counts when its bytes' mean length is over 4 grid
		 * steps; rounding alone at both ends gives a rigid edge the
		 * variance ev */
		for (e = 0; e < num_edges; e++)
		{
			double	sum = 0, sum2 = 0, byte_sum = 0, ev = 0, mean, var;

			for (t = 0; t < n; t++)
			{
				double	pa[3], pb[3], ba[3], bb[3], d[3], l2 = 0, lb2 = 0, lb, l;

				Position (m, z, sq->start + t, edges[e * 2], pa);
				Position (m, z, sq->start + t, edges[e * 2 + 1], pb);
				Position (m, NULL, sq->start + t, edges[e * 2], ba);
				Position (m, NULL, sq->start + t, edges[e * 2 + 1], bb);
				for (a = 0; a < 3; a++)
				{
					d[a] = ba[a] - bb[a];
					lb2 += d[a] * d[a];
					l2 += (pa[a] - pb[a]) * (pa[a] - pb[a]);
				}
				lb = sqrt (lb2);
				l = sqrt (l2);
				byte_sum += lb;
				sum += l;
				sum2 += l * l;
				if (lb > 1e-6)
				{
					for (a = 0; a < 3; a++)
						ev += (d[a] / lb) * (d[a] / lb) * 2.0 * m->scale[a] * m->scale[a] / 12.0;
				}
			}
			ev /= n;
			if (byte_sum / n <= 4 * maxs || ev <= 0)
				continue;
			mean = sum / n;
			var = q_max (sum2 / n - mean * mean, 0.0);
			if (num_ratios == max_ratios)
			{
				max_ratios = q_max (1024, max_ratios * 2);
				ratios = (double *) realloc (ratios, max_ratios * sizeof(double));
				if (!ratios)
					Sys_Error ("%s: out of memory", __thisfunc__);
			}
			ratios[num_ratios++] = sqrt (var / ev);
		}
	}
	*accel = acc_n ? sqrt (acc / acc_n) : 0;
	*edge_med = Percentile (ratios, num_ratios, 0.5);
	*edge_p90 = Percentile (ratios, num_ratios, 0.9);
	free (ratios);
	free (edges);
}

void VK_ModelSmoothStats (qmodel_t *model, const AliasTriangle *tris, int num_tris)
{
	const aliashdr_t	*hdr = (const aliashdr_t *) Mod_Extradata (model);
	qboolean		weld = r_smoothseams.integer != 0;
	model_t			m;
	smoothed_t		*s;
	uint16_t		*remap = (uint16_t *) malloc (hdr->poseverts * sizeof(uint16_t));
	double			**table, **rebuilt, **after, jt, jr, js, over_t, over_s, angle = 0;
	double			acc_b, acc_a, med_b, med_a, p90_b, p90_a, max_shift = 0;
	long			samples = 0, mismatch = 0;
	int			i, pose, v;
	qboolean		solved;

	if (!remap)
		Sys_Error ("%s: out of memory", __thisfunc__);
	InitModel (&m, hdr, tris, num_tris, remap);
	s = Smoothed (model, &m, &solved);

	table = AllNormals (&m, NULL, true, false);
	rebuilt = AllNormals (&m, NULL, false, weld);
	after = AllNormals (&m, s->z, false, weld);
	jt = NormalJerk (&m, table, &over_t);
	jr = NormalJerk (&m, rebuilt, NULL);
	js = NormalJerk (&m, after, &over_s);
	for (pose = 0; pose < m.num_poses; pose++)
	{
		for (v = 0; v < m.num_verts; v++)
		{
			double	d = DotProduct (&table[pose][v * 3], &rebuilt[pose][v * 3]);

			angle += acos (q_max (-1.0, q_min (1.0, d))) * (180.0 / M_PI);
		}
	}
	angle /= q_max (m.num_poses * m.num_verts, 1);
	for (i = 0; i < m.num_poses * m.num_verts * 3; i++)
	{
		double	q = Vert (&m, i / 3 / m.num_verts, (i / 3) % m.num_verts)->v[i % 3];

		samples++;
		if ((int) floor (s->z[i] + 0.5) != (int) q)
			mismatch++;
		max_shift = q_max (max_shift, fabs (s->z[i] - q));
	}
	ShapeStats (&m, NULL, &acc_b, &med_b, &p90_b);
	ShapeStats (&m, s->z, &acc_a, &med_a, &p90_a);

	Con_Printf ("%s: %d vertices (%d command vertices), %d poses, %d sequences (%d loops)\n", model->name,
		    m.num_verts, hdr->poseverts, m.num_poses, m.num_seqs, s->loops);
	Con_Printf ("  accel %.2f -> %.2f, edges median %.2f -> %.2f, 90th %.2f -> %.2f\n", acc_b, acc_a, med_b, med_a, p90_b, p90_a);
	Con_Printf ("  normal jerk (degrees) table %.2f / rebuilt %.2f / smoothed %.2f, above 5 degrees %.1f%% -> %.1f%%%s\n",
		    jt, jr, js, over_t, over_s, weld ? " (seams welded)" : "");
	Con_Printf ("  table vs rebuilt %.1f degrees; round trip %ld of %ld differ, largest shift %.3f\n", angle, mismatch, samples, max_shift);
	Con_Printf ("  solved in %.1f ms (this session's first build), %ld series, iterations mean %.0f max %d, %d not converged; "
		    "the engine uses %s normals\n", s->ms, s->series, s->series ? (double)s->iterations / s->series : 0.0,
		    s->max_iterations, s->not_converged, (!r_smoothmodels.integer || js > jt) ? "the table's" : "the rebuilt");

	FreeNormals (&m, table);
	FreeNormals (&m, rebuilt);
	FreeNormals (&m, after);
	FreeModel (&m);
	free (remap);
}

/* the session's smoothed models: count, memory, solve time */
void VK_ModelSmoothSummary (void)
{
	const smoothed_t	*s;
	double			ms = 0, bytes = 0;
	int			n = 0;

	for (s = smoothed; s; s = s->next)
	{
		n++;
		ms += s->ms;
		bytes += (double)s->num_poses * s->num_verts * 3 * sizeof(float);
	}
	Con_Printf ("smoothing (r_smoothmodels %d, r_smoothseams %d): %d models solved this session in %.0f ms, %.1f MB kept; "
		    "%d builds of models' poses in %.0f ms\n", r_smoothmodels.integer, r_smoothseams.integer, n, ms,
		    bytes / (1024.0 * 1024.0), builds, build_ms);
	if (last_presolve.models)
	{
		Con_Printf ("  the last threaded solve (a map load or the cvars): %d models on %d threads in %.0f ms\n", last_presolve.models,
			    last_presolve.threads, last_presolve.ms);
	}
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

static void SmoothChanged (cvar_t *var)
{
	(void)var;
	VK_RebuildModelGeometry ();
}

void VK_InitModelSmoothing (void)
{
	Cvar_RegisterVariable (&r_smoothmodels);
	Cvar_RegisterVariable (&r_smoothseams);
	Cvar_SetCallback (&r_smoothmodels, SmoothChanged);
	Cvar_SetCallback (&r_smoothseams, SmoothChanged);
}

void VK_ShutdownModelSmoothing (void)
{
	while (smoothed)
	{
		smoothed_t	*next = smoothed->next;

		free (smoothed->z);
		free (smoothed);
		smoothed = next;
	}
}
