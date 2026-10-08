/* mdl_smooth.cs -- story 8.1's prototype: smoother alias model animation
 *
 * An MDL stores each pose vertex as one byte per axis on one grid for the
 * whole model (scale, scale_origin), rounded on its own in every frame:
 * rigid parts shake by up to half a grid step between frames, and the
 * normals are one of Quake's 162 directions (anorms.h). For each animation
 * sequence (frames named alike but for a trailing number; a frame group's
 * subframes), each vertex's path on each axis becomes the smoothest one
 * (least squared second differences, across the wrap for loops) that stays
 * within its rounding cell, h grid steps around the stored byte (h < 0.5,
 * so rounding gives back the original bytes); normals are rebuilt from the
 * smoothed shape (the face normals around each vertex averaged, as the
 * original tools did: the table matches within its spacing). Solved
 * by ADMM per sequence: DtD + rho I factored once per sequence (an O(n)
 * solve per iteration), a clamp per sample. Faces are left out of the
 * normals as Rebuild says.
 *
 * Reports per model: the second differences before and after, the edge
 * lengths' variation against what rounding alone gives a rigid edge, the
 * normals' jerk (the table's, rebuilt from the bytes, rebuilt from the
 * smoothed shape), the round trip, the solver's time. Writes the viewer's
 * data (mdl_smooth_viewer.html). Used by mdl_smooth.ps1.
 *
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

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

public class MdlSeq
{
	public string Name;
	public int Start, Count;
	public bool Loop, Group;
	public double Wrap, Step;	/* last-to-first and mean frame-to-frame distance (model units) */
}

public class MdlModel
{
	public string Pak, Name;
	public bool Rapo;
	public float[] Scale = new float[3], Origin = new float[3];
	public int NumVerts, NumTris, NumPoses;
	public int[] Tris;		/* 3 per triangle, pose vertex indices */
	public List<string> PoseNames = new List<string>();
	public byte[] Q;		/* [pose][vertex][axis] */
	public byte[] NormalIndex;	/* [pose][vertex] */
	public List<MdlSeq> Seqs = new List<MdlSeq>();
	public double[] Z;		/* the smoothed positions, grid steps, like Q */
	public bool Flip;		/* the triangles wind against the table normals */
	public bool[] FaceKeep;		/* MdlSmooth.Prepare: back-to-back faces left out of the normals */
	public bool[] FaceKeepWeld;	/* the same with a seam's duplicates welded (Weld) */
	public int[] Rep;		/* MdlSmooth.Prepare: the vertex each one welds to (Weld) */
}

public class MdlStats
{
	public string Pak, Name;
	public int Verts, Poses, Seqs, Loops, Groups, Singles, Unsmoothed;
	public double AccelBefore, AccelAfter;		/* RMS second difference, grid steps */
	public double EdgeMedBefore, EdgeP90Before, EdgeMedAfter, EdgeP90After;
	public double NrmTable, NrmRebuilt, NrmSmoothed;	/* mean normal jerk, degrees */
	public double NrmTableOver5, NrmSmoothedOver5;	/* % of samples above 5 degrees */
	public double TableVsRebuilt;			/* mean angle, degrees */
	public long Samples, RoundTripMismatch;
	public double MaxShift;
	public double Ms, ItersMean;
	public int ItersMax, NotConverged;
}

public static class MdlSmooth
{
	public static double Rho = 1.0, Relax = 1.6, Eps = 1e-3, H = 0.49;
	public static int MaxIters = 2000;
	public static double LoopRatio = 1.5;
	public static string SeqFilter = "";	/* a regex on the sequences' names: only those smoothed and measured */
	public static double StepBelow = 0, StepAbove = 0;	/* > 0: only sequences whose mean frame-to-frame move is below (above) so many grid steps */

	static bool Selected(MdlModel m, MdlSeq sq)
	{
		double step = sq.Step / Math.Max(m.Scale[0], Math.Max(m.Scale[1], m.Scale[2]));
		return (SeqFilter == "" || Regex.IsMatch(sq.Name, SeqFilter)) && (StepBelow <= 0 || step < StepBelow) && (StepAbove <= 0 || step >= StepAbove);
	}

	public static bool AreaWeighted = false;	/* each face's unit normal counts the same: the table's normals were made so (8.1: the original game's median model 5.7 degrees from them, the table's spacing; area-weighted 10.3, all models) */
	static float[,] anorms;

	static int I(byte[] b, int o) { return BitConverter.ToInt32(b, o); }
	static float F(byte[] b, int o) { return BitConverter.ToSingle(b, o); }

	public static void LoadNormals(string path)
	{
		var nums = Regex.Matches(File.ReadAllText(path), @"\{\s*(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*\}");
		anorms = new float[nums.Count, 3];
		for (int i = 0; i < nums.Count; i++)
			for (int a = 0; a < 3; a++)
				anorms[i, a] = float.Parse(nums[i].Groups[a + 1].Value, CultureInfo.InvariantCulture);
	}

	public static int NumNormals { get { return anorms.GetLength(0); } }

	/* every .mdl in a pak whose name matches the filter (a regex, "" = all) */
	public static List<MdlModel> ReadPak(string pak, string filter)
	{
		var list = new List<MdlModel>();
		byte[] b = File.ReadAllBytes(pak);
		if (b.Length < 12 || Encoding.ASCII.GetString(b, 0, 4) != "PACK")
			throw new InvalidDataException(pak + ": not a pak file");
		int dirofs = I(b, 4), dirlen = I(b, 8);
		for (int o = dirofs; o < dirofs + dirlen; o += 64)
		{
			string name = Encoding.ASCII.GetString(b, o, 56).Split('\0')[0];
			if (!name.EndsWith(".mdl", StringComparison.OrdinalIgnoreCase) || (filter != "" && !Regex.IsMatch(name, filter)))
				continue;
			var m = Parse(b, I(b, o + 56), Path.GetFileName(pak), name);
			if (m != null)
				list.Add(m);
		}
		return list;
	}

	static MdlModel Parse(byte[] b, int p0, string pak, string name)
	{
		var m = new MdlModel { Pak = pak, Name = name };
		string ident = Encoding.ASCII.GetString(b, p0, 4);
		if ((ident != "IDPO" || I(b, p0 + 4) != 6) && (ident != "RAPO" || I(b, p0 + 4) != 50))
			return null;	/* ALIAS_VERSION, ALIAS_NEWVERSION */
		m.Rapo = ident == "RAPO";
		for (int a = 0; a < 3; a++)
		{
			m.Scale[a] = F(b, p0 + 8 + a * 4);
			m.Origin[a] = F(b, p0 + 20 + a * 4);
		}
		int numskins = I(b, p0 + 48), sw = I(b, p0 + 52), sh = I(b, p0 + 56);
		m.NumVerts = I(b, p0 + 60);
		m.NumTris = I(b, p0 + 64);
		int nf = I(b, p0 + 68);
		int nst = m.Rapo ? I(b, p0 + 84) : m.NumVerts;
		int p = p0 + (m.Rapo ? 88 : 84);
		for (int s = 0; s < numskins; s++)
		{
			if (I(b, p) == 0)
				p += 4 + sw * sh;
			else
			{
				int c = I(b, p + 4);
				p += 8 + c * 4 + c * sw * sh;
			}
		}
		p += nst * 12;
		m.Tris = new int[m.NumTris * 3];
		for (int t = 0; t < m.NumTris; t++)		/* dtriangle_t, dnewtriangle_t: 16 bytes */
			for (int k = 0; k < 3; k++)
				m.Tris[t * 3 + k] = m.Rapo ? BitConverter.ToUInt16(b, p + t * 16 + 4 + k * 2) : I(b, p + t * 16 + 4 + k * 4);
		p += m.NumTris * 16;

		var offsets = new List<int>();
		var groupOf = new List<int>();		/* -1 for a single frame */
		for (int f = 0; f < nf; f++)
		{
			if (I(b, p) == 0)
			{
				p += 4;
				m.PoseNames.Add(Encoding.ASCII.GetString(b, p + 8, 16).Split('\0')[0]);
				offsets.Add(p + 24);
				groupOf.Add(-1);
				p += 24 + m.NumVerts * 4;
			}
			else
			{
				int c = I(b, p + 4);
				p += 4 + 4 + 8 + c * 4;
				for (int k = 0; k < c; k++)
				{
					m.PoseNames.Add(Encoding.ASCII.GetString(b, p + 8, 16).Split('\0')[0]);
					offsets.Add(p + 24);
					groupOf.Add(f);
					p += 24 + m.NumVerts * 4;
				}
			}
		}
		m.NumPoses = offsets.Count;
		m.Q = new byte[m.NumPoses * m.NumVerts * 3];
		m.NormalIndex = new byte[m.NumPoses * m.NumVerts];
		for (int ps = 0; ps < m.NumPoses; ps++)
			for (int v = 0; v < m.NumVerts; v++)
			{
				for (int a = 0; a < 3; a++)
					m.Q[(ps * m.NumVerts + v) * 3 + a] = b[offsets[ps] + v * 4 + a];
				m.NormalIndex[ps * m.NumVerts + v] = b[offsets[ps] + v * 4 + 3];
			}

		/* sequences: a frame group's subframes (a loop), or runs of single
		 * frames named alike but for a trailing number */
		int start = 0;
		for (int ps = 1; ps <= m.NumPoses; ps++)
		{
			bool same;
			if (ps == m.NumPoses)
				same = false;
			else if (groupOf[start] >= 0 || groupOf[ps] >= 0)
				same = groupOf[ps] == groupOf[start];
			else
				same = Stem(m.PoseNames[ps]) == Stem(m.PoseNames[start]);
			if (same)
				continue;
			var sq = new MdlSeq { Name = Stem(m.PoseNames[start]), Start = start, Count = ps - start, Group = groupOf[start] >= 0 };
			Classify(m, sq);
			m.Seqs.Add(sq);
			start = ps;
		}
		return m;
	}

	static string Stem(string frame) { return Regex.Replace(frame.ToLowerInvariant(), "[0-9]+$", ""); }

	static double PoseDistance(MdlModel m, int pa, int pb)
	{
		double sum = 0;
		for (int v = 0; v < m.NumVerts; v++)
		{
			double d2 = 0;
			for (int a = 0; a < 3; a++)
			{
				double d = (m.Q[(pa * m.NumVerts + v) * 3 + a] - m.Q[(pb * m.NumVerts + v) * 3 + a]) * m.Scale[a];
				d2 += d * d;
			}
			sum += Math.Sqrt(d2);
		}
		return sum / Math.Max(m.NumVerts, 1);
	}

	/* a loop when the last pose is about as close to the first as the poses
	 * are to each other; a frame group always loops */
	static void Classify(MdlModel m, MdlSeq sq)
	{
		if (sq.Count < 2)
			return;
		double step = 0;
		for (int t = 0; t + 1 < sq.Count; t++)
			step += PoseDistance(m, sq.Start + t, sq.Start + t + 1);
		sq.Step = step / (sq.Count - 1);
		sq.Wrap = PoseDistance(m, sq.Start + sq.Count - 1, sq.Start);
		sq.Loop = sq.Group || sq.Wrap <= LoopRatio * sq.Step;
	}

	/* DtD + rho I = L Lt, D the second differences (cyclic for a loop): L
	 * row-major with each row's first nonzero column (Cholesky keeps the
	 * envelope: the band, and for a loop the last two rows), so a solve is
	 * O(n) */
	class Factor { public int N; public double[] L; public int[] First; }

	static Factor SolverFactor(int n, bool loop, double rho)
	{
		var a = new double[n, n];
		int rows = loop ? n : n - 2;
		for (int r = 0; r < rows; r++)
		{
			int[] idx = loop ? new[] { (r + n - 1) % n, r, (r + 1) % n } : new[] { r, r + 1, r + 2 };
			double[] w = { 1, -2, 1 };
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					a[idx[i], idx[j]] += w[i] * w[j];
		}
		for (int i = 0; i < n; i++)
			a[i, i] += rho;
		var f = new Factor { N = n, L = new double[n * n], First = new int[n] };
		for (int i = 0; i < n; i++)
		{
			int first = 0;
			while (a[i, first] == 0) first++;
			f.First[i] = first;
			for (int j = first; j <= i; j++)
			{
				double s = a[i, j];
				for (int k = Math.Max(first, f.First[j]); k < j; k++)
					s -= f.L[i * n + k] * f.L[j * n + k];
				f.L[i * n + j] = (i == j) ? Math.Sqrt(s) : s / f.L[j * n + j];
			}
		}
		return f;
	}

	/* x = (L Lt)^-1 y; y is overwritten */
	static void Solve(Factor f, double[] y, double[] x)
	{
		int n = f.N;
		var L = f.L;
		for (int i = 0; i < n; i++)
		{
			double s = y[i];
			for (int k = f.First[i]; k < i; k++)
				s -= L[i * n + k] * y[k];
			y[i] = s / L[i * n + i];
		}
		for (int i = n - 1; i >= 0; i--)
		{
			x[i] = y[i] / L[i * n + i];
			for (int k = f.First[i]; k < i; k++)
				y[k] -= L[i * n + k] * x[i];
		}
	}

	public static MdlStats Smooth(MdlModel m)
	{
		var st = new MdlStats { Pak = m.Pak, Name = m.Name, Verts = m.NumVerts, Poses = m.NumPoses, Seqs = m.Seqs.Count };
		Prepare(m);
		var sw = Stopwatch.StartNew();
		int nv = m.NumVerts;
		m.Z = new double[m.Q.Length];
		for (int i = 0; i < m.Q.Length; i++)
			m.Z[i] = m.Q[i];
		long iterSum = 0, series = 0;
		foreach (var sq in m.Seqs)
		{
			if (sq.Loop) st.Loops++;
			if (sq.Group) st.Groups++;
			if (sq.Count == 1) st.Singles++;
			if (sq.Count < 3) { if (sq.Count == 2) st.Unsmoothed++; continue; }
			if (!Selected(m, sq)) continue;
			int n = sq.Count;
			var fac = SolverFactor(n, sq.Loop, Rho);
			var q = new double[n]; var x = new double[n]; var z = new double[n]; var u = new double[n]; var y = new double[n];
			for (int v = 0; v < nv; v++)
				for (int a = 0; a < 3; a++)
				{
					bool moves = false;
					for (int t = 0; t < n; t++)
					{
						q[t] = m.Q[((sq.Start + t) * nv + v) * 3 + a];
						if (q[t] != q[0]) moves = true;
					}
					if (!moves)
						continue;	/* constant: already the smoothest */
					for (int t = 0; t < n; t++) { z[t] = q[t]; u[t] = 0; }
					int k;
					for (k = 0; k < MaxIters; k++)
					{
						for (int t = 0; t < n; t++)
							y[t] = Rho * (z[t] - u[t]);
						Solve(fac, y, x);
						double r = 0, dz = 0;
						for (int t = 0; t < n; t++)
						{
							double xr = Relax * x[t] + (1 - Relax) * z[t];
							double zn = Math.Min(Math.Max(xr + u[t], q[t] - H), q[t] + H);
							u[t] += xr - zn;
							r = Math.Max(r, Math.Abs(x[t] - zn));
							dz = Math.Max(dz, Math.Abs(zn - z[t]));
							z[t] = zn;
						}
						if (r < Eps && Rho * dz < Eps)
							break;
					}
					if (k == MaxIters) st.NotConverged++;
					iterSum += k + 1;
					series++;
					st.ItersMax = Math.Max(st.ItersMax, k + 1);
					for (int t = 0; t < n; t++)
						m.Z[((sq.Start + t) * nv + v) * 3 + a] = z[t];
				}
		}
		sw.Stop();
		st.Ms = sw.Elapsed.TotalMilliseconds;
		st.ItersMean = series > 0 ? (double)iterSum / series : 0;
		Measure(m, st);
		return st;
	}

	static double Pos(MdlModel m, double[] src, byte[] q, int pose, int v, int a)
	{
		int i = (pose * m.NumVerts + v) * 3 + a;
		return (src != null ? src[i] : q[i]) * m.Scale[a] + m.Origin[a];
	}

	/* the face normals around each vertex of one pose, averaged; the sign
	 * flipped when flip (the triangles' winding against the table's). With
	 * filter (all but the winding test), a face below MinFace grid cells
	 * counts by its area (collapsed triangles, which smoothing opens by a
	 * fraction of a step: their direction is noise; continuous, so a face
	 * doesn't drop in and out between frames), and of back-to-back faces
	 * (on the same three vertices: the flag, the seaweed, plants) only the
	 * one Prepare kept; a vertex whose faces weigh less than one full face
	 * takes the rest from its table normal (the decapitated medusa's head,
	 * collapsed to a point). With Weld, vertices at the same place in every
	 * pose (a UV seam's duplicates, not a card's two sides: Prepare) share
	 * one normal: the original game's
	 * tables are each duplicate's own side, Portal of Praevus' models welded
	 * them and kept one side's (8.1) */
	public static double MinFace = 0.25;
	public static bool Weld = false;

	static double[] Rebuild(MdlModel m, double[] src, int pose, bool flip, bool filter = true)
	{
		var n = new double[m.NumVerts * 3];
		var weight = new double[m.NumVerts];
		var p = new double[3, 3];
		double s = Math.Max(m.Scale[0], Math.Max(m.Scale[1], m.Scale[2]));
		double fullCross = 2 * MinFace * s * s;	/* twice the area of a face that counts fully */
		for (int t = 0; t < m.NumTris; t++)
		{
			if (filter && m.FaceKeep != null && !(Weld ? m.FaceKeepWeld : m.FaceKeep)[t])
				continue;
			for (int k = 0; k < 3; k++)
				for (int a = 0; a < 3; a++)
					p[k, a] = Pos(m, src, m.Q, pose, m.Tris[t * 3 + k], a);
			double e1x = p[1, 0] - p[0, 0], e1y = p[1, 1] - p[0, 1], e1z = p[1, 2] - p[0, 2];
			double e2x = p[2, 0] - p[0, 0], e2y = p[2, 1] - p[0, 1], e2z = p[2, 2] - p[0, 2];
			double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
			if (flip) { cx = -cx; cy = -cy; cz = -cz; }
			double l = Math.Sqrt(cx * cx + cy * cy + cz * cz);
			double w = 1.0;
			if (l == 0)
				continue;
			if (!AreaWeighted)
			{
				w = filter ? Math.Min(1.0, l / fullCross) : 1.0;
				cx *= w / l; cy *= w / l; cz *= w / l;
			}
			for (int k = 0; k < 3; k++)
			{
				int v = m.Tris[t * 3 + k];
				if (filter && Weld) v = m.Rep[v];
				n[v * 3] += cx; n[v * 3 + 1] += cy; n[v * 3 + 2] += cz;
				weight[v] += w;
			}
		}
		var tn = TableNormals(m, pose);
		for (int v = 0; v < m.NumVerts; v++)
		{
			if (filter && Weld && m.Rep[v] != v)
				continue;	/* copied from its representative below */
			if (filter && !AreaWeighted && weight[v] < 1)	/* less than a full face: the table fills in */
				for (int a = 0; a < 3; a++) n[v * 3 + a] += (1 - weight[v]) * tn[v * 3 + a];
			double l = Math.Sqrt(n[v * 3] * n[v * 3] + n[v * 3 + 1] * n[v * 3 + 1] + n[v * 3 + 2] * n[v * 3 + 2]);
			if (l > 0) { n[v * 3] /= l; n[v * 3 + 1] /= l; n[v * 3 + 2] /= l; }
			else for (int a = 0; a < 3; a++) n[v * 3 + a] = tn[v * 3 + a];
		}
		if (filter && Weld)
			for (int v = 0; v < m.NumVerts; v++)
				for (int a = 0; a < 3; a++)
					n[v * 3 + a] = n[m.Rep[v] * 3 + a];
		return n;
	}

	static double[] TableNormals(MdlModel m, int pose)
	{
		var n = new double[m.NumVerts * 3];
		for (int v = 0; v < m.NumVerts; v++)
		{
			int i = Math.Min(m.NormalIndex[pose * m.NumVerts + v], NumNormals - 1);
			for (int a = 0; a < 3; a++)
				n[v * 3 + a] = anorms[i, a];
		}
		return n;
	}

	public static bool WindingFlipped(MdlModel m)
	{
		var r = Rebuild(m, null, 0, false, false);
		var tn = TableNormals(m, 0);
		double d = 0;
		for (int i = 0; i < r.Length; i++)
			d += r[i] * tn[i];
		return d < 0;
	}

	/* the winding, the welded vertices (Weld: the same bytes in every pose,
	 * a UV seam's duplicates or, rarely, parts that touch throughout), and of
	 * faces on the same three vertices the one KeepFaces keeps, welded or
	 * not: decided once per model, so it doesn't change between frames */
	public static void Prepare(MdlModel m)
	{
		m.Flip = WindingFlipped(m);
		/* Weld: the first vertex at the same place in every pose */
		m.Rep = new int[m.NumVerts];
		var at = new Dictionary<string, int>();
		for (int v = 0; v < m.NumVerts; v++)
		{
			var key = new StringBuilder(m.NumPoses * 3);
			for (int ps = 0; ps < m.NumPoses; ps++)
				for (int a = 0; a < 3; a++)
					key.Append((char)m.Q[(ps * m.NumVerts + v) * 3 + a]);
			string k = key.ToString();
			if (!at.ContainsKey(k)) at[k] = v;
			m.Rep[v] = at[k];
			/* nor sides facing away from each other (a card triangulated
			 * differently on each side: the scarab's wings) */
			if (m.Rep[v] != v && TableAgreement(m, v, m.Rep[v]) < -0.5)
				m.Rep[v] = v;
		}
		/* not the two sides of a card: faces that lie on each other only
		 * once welded (the plants, the scarab's wings) keep their vertices
		 * apart, else the sides' normals cancel */
		var byWelded = new Dictionary<string, HashSet<string>>();
		var faceOf = new Dictionary<string, List<int>>();
		for (int t = 0; t < m.NumTris; t++)
		{
			string raw = string.Join(",", new[] { m.Tris[t * 3], m.Tris[t * 3 + 1], m.Tris[t * 3 + 2] }.OrderBy(i => i));
			string wel = string.Join(",", new[] { m.Rep[m.Tris[t * 3]], m.Rep[m.Tris[t * 3 + 1]], m.Rep[m.Tris[t * 3 + 2]] }.OrderBy(i => i));
			if (!byWelded.ContainsKey(wel)) { byWelded[wel] = new HashSet<string>(); faceOf[wel] = new List<int>(); }
			byWelded[wel].Add(raw);
			faceOf[wel].Add(t);
		}
		var cards = new HashSet<int>();
		foreach (var kv in byWelded)
			if (kv.Value.Count > 1)
				foreach (int t in faceOf[kv.Key])
					for (int k = 0; k < 3; k++)
						cards.Add(m.Rep[m.Tris[t * 3 + k]]);
		for (int v = 0; v < m.NumVerts; v++)
			if (cards.Contains(m.Rep[v]))
				m.Rep[v] = v;
		m.FaceKeep = KeepFaces(m, v => v);
		m.FaceKeepWeld = KeepFaces(m, v => m.Rep[v]);
	}

	/* of faces on the same three vertices (map: the vertices as they are, or
	 * welded: back-to-back faces on different duplicates) the one that
	 * agrees best with the table's normals at them over all poses */
	static bool[] KeepFaces(MdlModel m, Func<int, int> map)
	{
		var keep = new bool[m.NumTris];
		var groups = new Dictionary<string, List<int>>();
		for (int t = 0; t < m.NumTris; t++)
		{
			keep[t] = true;
			var key = string.Join(",", new[] { map(m.Tris[t * 3]), map(m.Tris[t * 3 + 1]), map(m.Tris[t * 3 + 2]) }.OrderBy(i => i));
			if (!groups.ContainsKey(key)) groups[key] = new List<int>();
			groups[key].Add(t);
		}
		foreach (var g in groups.Values)
		{
			if (g.Count < 2) continue;
			int best = g[0]; double bestScore = double.NegativeInfinity;
			foreach (int t in g)
			{
				double score = 0;
				for (int ps = 0; ps < m.NumPoses; ps++)
				{
					var p = new double[3, 3];
					for (int k = 0; k < 3; k++)
						for (int a = 0; a < 3; a++)
							p[k, a] = Pos(m, null, m.Q, ps, m.Tris[t * 3 + k], a);
					double e1x = p[1, 0] - p[0, 0], e1y = p[1, 1] - p[0, 1], e1z = p[1, 2] - p[0, 2];
					double e2x = p[2, 0] - p[0, 0], e2y = p[2, 1] - p[0, 1], e2z = p[2, 2] - p[0, 2];
					double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
					if (m.Flip) { cx = -cx; cy = -cy; cz = -cz; }
					for (int k = 0; k < 3; k++)
					{
						int i = Math.Min(m.NormalIndex[ps * m.NumVerts + m.Tris[t * 3 + k]], NumNormals - 1);
						score += cx * anorms[i, 0] + cy * anorms[i, 1] + cz * anorms[i, 2];
					}
				}
				if (score > bestScore) { bestScore = score; best = t; }
			}
			foreach (int t in g)
				keep[t] = t == best;
		}
		return keep;
	}

	/* the mean dot product of two vertices' table normals over all poses */
	static double TableAgreement(MdlModel m, int a, int b)
	{
		double d = 0;
		for (int ps = 0; ps < m.NumPoses; ps++)
		{
			int i = Math.Min(m.NormalIndex[ps * m.NumVerts + a], NumNormals - 1);
			int j = Math.Min(m.NormalIndex[ps * m.NumVerts + b], NumNormals - 1);
			d += anorms[i, 0] * anorms[j, 0] + anorms[i, 1] * anorms[j, 1] + anorms[i, 2] * anorms[j, 2];
		}
		return d / Math.Max(m.NumPoses, 1);
	}

	static double Angle(double[] a, int i, double bx, double by, double bz)
	{
		double l = Math.Sqrt(bx * bx + by * by + bz * bz);
		if (l < 1e-9) return 0;
		double d = (a[i] * bx + a[i + 1] * by + a[i + 2] * bz) / l;
		return Math.Acos(Math.Max(-1, Math.Min(1, d))) * 180 / Math.PI;
	}

	static double Percentile(List<double> l, double p)
	{
		if (l.Count == 0) return 0;
		l.Sort();
		return l[Math.Min((int)(l.Count * p), l.Count - 1)];
	}

	static void Measure(MdlModel m, MdlStats st)
	{
		int nv = m.NumVerts;
		bool flip = m.Flip;
		double accB = 0, accA = 0; long accN = 0;
		var edges = new HashSet<long>();
		for (int t = 0; t < m.NumTris; t++)
			for (int k = 0; k < 3; k++)
			{
				int a = m.Tris[t * 3 + k], c = m.Tris[t * 3 + (k + 1) % 3];
				edges.Add(((long)Math.Min(a, c) << 32) | (uint)Math.Max(a, c));
			}
		var ratB = new List<double>(); var ratA = new List<double>();
		double jt = 0, jr = 0, js = 0; long jn = 0, jtOver = 0, jsOver = 0;
		double tvr = 0; long tvrN = 0;
		float maxScale = Math.Max(m.Scale[0], Math.Max(m.Scale[1], m.Scale[2]));

		/* the table against normals rebuilt from the bytes, every pose */
		for (int ps = 0; ps < m.NumPoses; ps++)
		{
			var tn = TableNormals(m, ps); var rn = Rebuild(m, null, ps, flip);
			for (int v = 0; v < nv; v++, tvrN++)
				tvr += Angle(tn, v * 3, rn[v * 3], rn[v * 3 + 1], rn[v * 3 + 2]);
		}
		st.TableVsRebuilt = tvrN > 0 ? tvr / tvrN : 0;

		foreach (var sq in m.Seqs)
		{
			int n = sq.Count;
			if (n < 3 || !Selected(m, sq)) continue;
			/* second differences, grid steps */
			int rows = sq.Loop ? n : n - 2;
			for (int v = 0; v < nv; v++)
				for (int a = 0; a < 3; a++)
					for (int r = 0; r < rows; r++)
					{
						int t0 = sq.Loop ? (r + n - 1) % n : r, t1 = sq.Loop ? r : r + 1, t2 = sq.Loop ? (r + 1) % n : r + 2;
						int i0 = ((sq.Start + t0) * nv + v) * 3 + a, i1 = ((sq.Start + t1) * nv + v) * 3 + a, i2 = ((sq.Start + t2) * nv + v) * 3 + a;
						double db = m.Q[i0] - 2.0 * m.Q[i1] + m.Q[i2], da = m.Z[i0] - 2 * m.Z[i1] + m.Z[i2];
						accB += db * db; accA += da * da; accN++;
					}
			/* edge lengths against rounding alone on a rigid edge */
			if (n >= 4)
				foreach (long e in edges)
				{
					int va = (int)(e >> 32), vb = (int)(e & 0xffffffff);
					double sB = 0, s2B = 0, sA = 0, s2A = 0, ev = 0;
					for (int t = 0; t < n; t++)
					{
						double l2B = 0, l2A = 0; var d = new double[3];
						for (int a = 0; a < 3; a++)
						{
							d[a] = Pos(m, null, m.Q, sq.Start + t, va, a) - Pos(m, null, m.Q, sq.Start + t, vb, a);
							double dA = Pos(m, m.Z, null, sq.Start + t, va, a) - Pos(m, m.Z, null, sq.Start + t, vb, a);
							l2B += d[a] * d[a]; l2A += dA * dA;
						}
						double lB = Math.Sqrt(l2B), lA = Math.Sqrt(l2A);
						sB += lB; s2B += lB * lB; sA += lA; s2A += lA * lA;
						if (lB > 1e-6)
							for (int a = 0; a < 3; a++)
								ev += (d[a] / lB) * (d[a] / lB) * 2.0 * m.Scale[a] * m.Scale[a] / 12.0;
					}
					double mean = sB / n; ev /= n;
					if (mean <= 4 * maxScale || ev <= 0) continue;
					ratB.Add(Math.Sqrt(Math.Max(s2B / n - mean * mean, 0) / ev));
					ratA.Add(Math.Sqrt(Math.Max(s2A / n - (sA / n) * (sA / n), 0) / ev));
				}
			/* normal jerk: n[t] against the direction halfway between its neighbours */
			var tab = new double[n][]; var reb = new double[n][]; var smo = new double[n][];
			for (int t = 0; t < n; t++)
			{
				tab[t] = TableNormals(m, sq.Start + t);
				reb[t] = Rebuild(m, null, sq.Start + t, flip);
				smo[t] = Rebuild(m, m.Z, sq.Start + t, flip);
			}
			for (int r = 0; r < rows; r++)
			{
				int t0 = sq.Loop ? (r + n - 1) % n : r, t1 = sq.Loop ? r : r + 1, t2 = sq.Loop ? (r + 1) % n : r + 2;
				for (int v = 0; v < nv; v++)
				{
					int i = v * 3;
					double a1 = Angle(tab[t1], i, tab[t0][i] + tab[t2][i], tab[t0][i + 1] + tab[t2][i + 1], tab[t0][i + 2] + tab[t2][i + 2]);
					double a2 = Angle(reb[t1], i, reb[t0][i] + reb[t2][i], reb[t0][i + 1] + reb[t2][i + 1], reb[t0][i + 2] + reb[t2][i + 2]);
					double a3 = Angle(smo[t1], i, smo[t0][i] + smo[t2][i], smo[t0][i + 1] + smo[t2][i + 1], smo[t0][i + 2] + smo[t2][i + 2]);
					jt += a1; jr += a2; js += a3; jn++;
					if (a1 > 5) jtOver++;
					if (a3 > 5) jsOver++;
				}
			}
		}
		st.AccelBefore = accN > 0 ? Math.Sqrt(accB / accN) : 0;
		st.AccelAfter = accN > 0 ? Math.Sqrt(accA / accN) : 0;
		st.EdgeMedBefore = Percentile(ratB, 0.5); st.EdgeP90Before = Percentile(ratB, 0.9);
		st.EdgeMedAfter = Percentile(ratA, 0.5); st.EdgeP90After = Percentile(ratA, 0.9);
		st.NrmTable = jn > 0 ? jt / jn : 0; st.NrmRebuilt = jn > 0 ? jr / jn : 0; st.NrmSmoothed = jn > 0 ? js / jn : 0;
		st.NrmTableOver5 = jn > 0 ? 100.0 * jtOver / jn : 0; st.NrmSmoothedOver5 = jn > 0 ? 100.0 * jsOver / jn : 0;

		/* the round trip: rounding the smoothed positions gives the bytes */
		for (int i = 0; i < m.Q.Length; i++)
		{
			st.Samples++;
			if ((int)Math.Round(m.Z[i], MidpointRounding.AwayFromZero) != m.Q[i]) st.RoundTripMismatch++;
			st.MaxShift = Math.Max(st.MaxShift, Math.Abs(m.Z[i] - m.Q[i]));
		}
	}

	/* the viewer's data: one JSON object per model */
	/* a JSON string, also safe inside a script element */
	static string Js(string s)
	{
		var sb = new StringBuilder("\"");
		foreach (char c in s)
			if (c == '"' || c == '\\' || c < 0x20 || c == '<' || c > 0x7e)
				sb.AppendFormat("\\u{0:x4}", (int)c);
			else
				sb.Append(c);
		return sb.Append('"').ToString();
	}

	public static string ViewerJson(MdlModel m)
	{
		var sb = new StringBuilder();
		var inv = CultureInfo.InvariantCulture;
		sb.Append("{\"name\":").Append(Js(m.Pak + ":" + m.Name));
		sb.Append(",\"scale\":[").Append(string.Join(",", m.Scale.Select(f => f.ToString("R", inv)))).Append(']');
		sb.Append(",\"origin\":[").Append(string.Join(",", m.Origin.Select(f => f.ToString("R", inv)))).Append(']');
		sb.Append(",\"verts\":").Append(m.NumVerts).Append(",\"poses\":").Append(m.NumPoses);
		sb.Append(",\"flip\":").Append(m.Flip ? "true" : "false");
		sb.Append(",\"keep\":\"").Append(Convert.ToBase64String(m.FaceKeep.Select(k => k ? (byte)1 : (byte)0).ToArray())).Append('"');
		sb.Append(",\"keepWeld\":\"").Append(Convert.ToBase64String(m.FaceKeepWeld.Select(k => k ? (byte)1 : (byte)0).ToArray())).Append('"');
		var rb = new byte[m.NumVerts * 2];
		for (int i = 0; i < m.NumVerts; i++) { rb[i * 2] = (byte)(m.Rep[i] & 255); rb[i * 2 + 1] = (byte)(m.Rep[i] >> 8); }
		sb.Append(",\"rep\":\"").Append(Convert.ToBase64String(rb)).Append('"');
		var tb = new byte[m.Tris.Length * 2];
		for (int i = 0; i < m.Tris.Length; i++) { tb[i * 2] = (byte)(m.Tris[i] & 255); tb[i * 2 + 1] = (byte)(m.Tris[i] >> 8); }
		sb.Append(",\"tris\":\"").Append(Convert.ToBase64String(tb)).Append('"');
		sb.Append(",\"q\":\"").Append(Convert.ToBase64String(m.Q)).Append('"');
		var d = new byte[m.Q.Length];
		for (int i = 0; i < d.Length; i++)
			d[i] = (byte)(sbyte)Math.Round((m.Z[i] - m.Q[i]) * 250);	/* 1/250 grid step */
		sb.Append(",\"d\":\"").Append(Convert.ToBase64String(d)).Append('"');
		sb.Append(",\"n\":\"").Append(Convert.ToBase64String(m.NormalIndex)).Append('"');
		sb.Append(",\"seqs\":[");
		sb.Append(string.Join(",", m.Seqs.Select(s => string.Format(inv, "{{\"name\":{0},\"start\":{1},\"count\":{2},\"loop\":{3}}}",
			Js(s.Name), s.Start, s.Count, s.Loop ? "true" : "false"))));
		sb.Append("]}");
		return sb.ToString();
	}

	public static string NormalsJson()
	{
		var inv = CultureInfo.InvariantCulture;
		var parts = new List<string>();
		for (int i = 0; i < NumNormals; i++)
			for (int a = 0; a < 3; a++)
				parts.Add(anorms[i, a].ToString("R", inv));
		return "[" + string.Join(",", parts) + "]";
	}
}
