/* Copyright (C) 2003 Massachusetts Institute of Technology
%
%  This program is free software; you can redistribute it and/or modify
%  it under the terms of the GNU General Public License as published by
%  the Free Software Foundation; either version 2, or (at your option)
%  any later version.
%
%  This program is distributed in the hope that it will be useful,
%  but WITHOUT ANY WARRANTY; without even the implied warranty of
%  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
%  GNU General Public License for more details.
%
%  You should have received a copy of the GNU General Public License
%  along with this program; if not, write to the Free Software Foundation,
%  Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "dactyl.h"
#include "dactyl_internals.h"

mat::mat() {
  num_chunks = 0;
  outdir = ".";
  S = identity();
}

mat::mat(const volume &thev, double eps(const vec &), int num, const symmetry &s) {
  outdir = ".";
  if (num == 0) num = count_processors();
  choose_chunkdivision(thev, eps, num, s);
}

void mat::choose_chunkdivision(const volume &thev, double eps(const vec &),
                               int num, const symmetry &s) {
  num_chunks = num;
  user_volume = thev;
  v = thev;
  S = s;
  if (S.multiplicity() > 1) {
    // Have to work out the symmetry point and volume to use.
    if (!(thev.dim == D2 /* || thev.dim == D3 */))
      abort("I don't support symmetries except in cartesian.  %s\n",
            dimension_name(thev.dim));
    bool break_this[3];
    for (int dd=0;dd<3;dd++) {
      const direction d = (direction) dd;
      break_this[d] = false;
      for (int n=0;n<S.multiplicity();n++)
        if (has_direction(thev.dim,(direction)d) && (S.transform(d,n).d != d || S.transform(d,n).flipped)) {
          break_this[d] = true;
          if (thev.num_direction(d) & 1)
            abort("Aaack, odd number of grid points!\n");
        }
    }
    for (int d=0;d<3;d++)
      if (break_this[d]) v = v.split_specifically(2,0,(direction)d);
    // Pad the little cell in any direction that we've shrunk:
    for (int d=0;d<3;d++)
      if (break_this[d]) v = v.pad((direction)d);
  }
  chunks = new (mat_chunk *)[num_chunks];
  for (int i=0;i<num_chunks;i++) {
    const int proc = i*count_processors()/num_chunks;
    chunks[i] = new mat_chunk( v.split(num_chunks,i), eps, proc);
  }
}

mat::mat(const mat *m) {
  num_chunks = m->num_chunks;
  outdir = m->outdir;
  v = m->v;
  S = m->S;
  user_volume = m->user_volume;
  chunks = new (mat_chunk *)[num_chunks];
  for (int i=0;i<num_chunks;i++) chunks[i] = new mat_chunk(m->chunks[i]);
}

mat::mat(const mat &m) {
  num_chunks = m.num_chunks;
  outdir = m.outdir;
  v = m.v;
  S = m.S;
  user_volume = m.user_volume;
  chunks = new (mat_chunk *)[num_chunks];
  for (int i=0;i<num_chunks;i++) chunks[i] = new mat_chunk(m.chunks[i]);
  
}

mat::~mat() {
  for (int i=0;i<num_chunks;i++) {
    delete chunks[i];
    chunks[i] = NULL; // Just to be sure...
  }
  delete[] chunks;
}

void mat::make_average_eps() {
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      chunks[i]->make_average_eps(); // FIXME
}

void mat::use_pml(direction d, boundary_side b, double dx) {
  for (int i=0;i<num_chunks;i++)
    chunks[i]->use_pml(d, dx, user_volume.boundary_location(b,d));
}

void mat::use_pml_everywhere(double dx) {
  for (int b=0;b<2;b++) FOR_DIRECTIONS(d)
    if (user_volume.has_boundary((boundary_side)b, d))
      use_pml(d, (boundary_side)b, dx);
}

void mat::mix_with(const mat *oth, double f) {
  if (num_chunks != oth->num_chunks)
    abort("You can't phase materials with different chunk topologies...\n");
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      chunks[i]->mix_with(oth->chunks[i], f);
}

mat_chunk::~mat_chunk() {
  FOR_ELECTRIC_COMPONENTS(c)
    FOR_DIRECTIONS(d)
      delete[] inveps[c][d];
  delete[] eps;

  FOR_ELECTRIC_COMPONENTS(c) FOR_DIRECTIONS(d) delete[] C[d][c];
  FOR_ELECTRIC_COMPONENTS(c)
    FOR_DIRECTIONS(d) FOR_DIRECTIONS(d2)
        delete[] Cdecay[d][c][d2];
  if (pb) delete pb;
}

static double sig(double r, double power);

static double minimize_badness(double sig[], int thickness, double eps, double fmin, int i);
inline void reverse(double sig[], int l) {
  for (int i=0;i<l/2;i++) {
    double temp = sig[i];
    sig[i] = sig[l-1-i];
    sig[l-1-i] = temp;
  }
}

static double badness(double sig[], int thickness, double epsilon, double fmin) {
  if (thickness < 1) return 1;
  const double A = .0001/fmin*.1/fmin, K = 6.0/epsilon*2.25/epsilon;
  double sofar = 1.0;
  for (int i=0;i<thickness-1;i++) {
    double first_trans = exp(-K*sig[i+1]);
    double refl = A*fabs(sig[i]-sig[i+1])*fabs(sig[i]-sig[i+1]);
    double total_trans = exp(-K*sig[i])*first_trans;
    sofar = refl + (1-refl)*total_trans*sofar;
    if (sofar > 1.0) sofar = 1.0;
  }
  double last_refl = A*fabs(sig[thickness-1]);
  sofar = last_refl + (1-last_refl)*sofar;
  return sofar;
}

static double minimize_badness(double sig[], int thickness,
                               double epsilon, double fmin, int i) {
  double behind_reflection = badness(sig, i-1, epsilon, fmin);
  

  double now = badness(sig, thickness, epsilon, fmin);
  double tried = now;
  do {
    now = tried;
    sig[i] *= 1.001;
    tried = badness(sig, thickness, epsilon, fmin);
  } while (tried < now);
  sig[i] /= 1.001;
  tried = now = badness(sig, thickness, epsilon, fmin);
  do {
    now = tried;
    sig[i] /= 1.001;
    tried = badness(sig, thickness, epsilon, fmin);
  } while (tried < now);
  sig[i] *= 1.001;
  return badness(sig, thickness, epsilon, fmin);
}

static double sig(double r, double power) {
  return pow(r, power);
}

void mat_chunk::mix_with(const mat_chunk *n, double f) {
  for (int i=0;i<v.ntot();i++)
    eps[i] = 1.0/(1.0/eps[i] + f*(1.0/n->eps[i]-1.0/eps[i]));
  FOR_ELECTRIC_COMPONENTS(c) FOR_DIRECTIONS(d)
    if (inveps[c][d])
      for (int i=0;i<v.ntot();i++)
        inveps[c][d][i] += f*(n->inveps[c][d][i] - inveps[c][d][i]);
  // Mix in the polarizability...
  polarizability *po = pb, *pn = n->pb;
  while (po && pn) {
    for (int c=0;c<10;c++)
      if (v.has_field((component)c) && is_electric((component)c))
        for (int i=0;i<v.ntot();i++)
          po->s[c][i] += f*(pn->s[c][i] - po->s[c][i]);
    po = po->next;
    pn = pn->next;
  }
}

void mat_chunk::make_average_eps() {
  double meaneps = 0;
  for (int i=0;i<v.ntot();i++) {
    meaneps += eps[i]; // This is totally wrong, as it needs parallelization.
  }
  meaneps /= v.ntot();
  for (int i=0;i<v.ntot();i++)
    eps[i] = meaneps;
  FOR_ELECTRIC_COMPONENTS(c)
    if (v.has_field(c))
      for (int i=0;i<v.ntot();i++)
        inveps[c][component_direction(c)][i] = 1/meaneps;
}

const double Cmax = 0.5;

void mat_chunk::use_pml(direction d, double dx, double bloc) {
  const double prefac = Cmax/(dx*dx);
  // Don't bother with PML if we don't even overlap with the PML region...
  if (bloc > v.boundary_location(High,d) + dx + 1.0/a ||
      bloc < v.boundary_location(Low,d) - dx - 1.0/a) return;
  if (is_mine()) {
    FOR_COMPONENTS(c)
      if (v.has_field(c) && component_direction(c) != d) {
        if (!C[d][c]) {
          C[d][c] = new double[v.ntot()];
          for (int i=0;i<v.ntot();i++) C[d][c][i] = 0.0;
        }
        for (int i=0;i<v.ntot();i++) {
          const double x = dx - (0.5/a)*
            ((int)(2*a*fabs(bloc-v.loc((component)c,i).in_direction(d))+0.5));
          if (x > 0) C[d][c][i] = prefac*x*x;
        }
      }
    FOR_COMPONENTS(c) FOR_DIRECTIONS(d2)
      if ((inveps[c][d2] || d2 == component_direction(c)) &&
          C[d][c] && d2 != d) {
        if (!Cdecay[d][c][d2]) {
          Cdecay[d][c][d2] = new double[v.ntot()];
          if (is_electric(c))
            for (int i=0;i<v.ntot();i++) Cdecay[d][c][d2][i] = inveps[c][d2][i];
          else
            for (int i=0;i<v.ntot();i++) Cdecay[d][c][d2][i] = 1.0;
        }
        for (int i=0;i<v.ntot();i++) {
          if (is_magnetic(c)) Cdecay[d][c][d2][i] = 1.0/(1.0+0.5*C[d][c][i]);
          else Cdecay[d][c][d2][i] =
                 inveps[c][d2][i]/(1.0+0.5*C[d][c][i]*inveps[c][d2][i]);
        }
      }
  }
}

mat_chunk::mat_chunk(const mat_chunk *o) {
  if (o->pb) pb = new polarizability(o->pb);
  else pb = NULL;
  a = o->a;
  v = o->v;
  the_proc = o->the_proc;
  the_is_mine = my_rank() == n_proc();
  if (is_mine()) {
    eps = new double[v.ntot()];
    if (eps == NULL) abort("Out of memory!\n");
    for (int i=0;i<v.ntot();i++) eps[i] = o->eps[i];
  } else {
    eps = NULL;
  }
  FOR_COMPONENTS(c) FOR_DIRECTIONS(d)
    if (is_mine() && o->inveps[c][d]) {
      inveps[c][d] = new double[v.ntot()];
      for (int i=0;i<v.ntot();i++) inveps[c][d][i] = o->inveps[c][d][i];
    } else {
      inveps[c][d] = NULL;
    }
  // Allocate the conductivity arrays:
  FOR_DIRECTIONS(d) FOR_COMPONENTS(c) C[d][c] = NULL;
  FOR_DIRECTIONS(d) FOR_COMPONENTS(c) FOR_DIRECTIONS(d2)
    Cdecay[d][c][d2] = NULL;
  // Copy over the conductivity arrays:
  if (is_mine())
    FOR_DIRECTIONS(d) FOR_COMPONENTS(c)
      if (o->C[d][c]) {
        C[d][c] = new double[v.ntot()];
        for (int i=0;i<v.ntot();i++) C[d][c][i] = o->C[d][c][i];
        FOR_DIRECTIONS(d2)
          if (o->Cdecay[d][c][d2]) {
            Cdecay[d][c][d2] = new double[v.ntot()];
            for (int i=0;i<v.ntot();i++)
              Cdecay[d][c][d2][i] = o->Cdecay[d][c][d2][i];
          }
      }
}

mat_chunk::mat_chunk(const volume &thev, double feps(const vec &), int pr) {
  pml_fmin = 0.2;
  pb = NULL;
  v = thev;
  a = thev.a;
  the_proc = pr;
  the_is_mine = n_proc() == my_rank();
  if (is_mine()) {
    eps = new double[v.ntot()];
    if (eps == NULL) abort("Out of memory!\n");
    for (int i=0;i<v.ntot();i++) eps[i] = feps(v.loc(v.eps_component(),i));
  } else {
    eps = NULL;
  }
  FOR_COMPONENTS(c) FOR_DIRECTIONS(d)
    if (is_mine() && v.has_field(c) && is_electric(c) &&
        d == component_direction(c)) {
      inveps[c][d] = new double[v.ntot()];
      // Initialize eps to 1;
      for (int i=0;i<v.ntot();i++) inveps[c][d][i] = 1;
    } else {
      inveps[c][d] = NULL;
    }
  if (is_mine())
    if (v.dim == Dcyl) {
      const vec dr = v.dr()*0.5; // The distance between Yee field components
      const vec dz = v.dz()*0.5; // The distance between Yee field components
      for (int i=0;i<v.ntot();i++) {
        const vec here = v.loc(Ep,i);
        inveps[Er][R][i] = 2./(feps(here+dr+dz) + feps(here+dr-dz));
        inveps[Ep][P][i] = 4./(feps(here+dr+dz) + feps(here-dr+dz) +
                               feps(here+dr-dz) + feps(here-dr-dz));
        inveps[Ez][Z][i] = 2./(feps(here+dr+dz) + feps(here-dr+dz));
      }
    } else if (v.dim == D1) {
      for (int i=0;i<v.ntot();i++) inveps[Ex][X][i] = 1.0/eps[i];
    } else if (v.dim == D2) {
      if (inveps[Ez][Z])
        for (int i=0;i<v.ntot();i++) inveps[Ez][Z][i] = 1.0/eps[i];
      const vec hdx = v.dx()*0.5;;
      if (inveps[Ex][X])
        for (int i=0;i<v.ntot();i++) {
          const vec here = v.loc(Ex,i);
          inveps[Ex][X][i] = 2.0/(feps(here+hdx)+feps(here-hdx));
        }
      const vec hdy = v.dy()*0.5;;
      if (inveps[Ey][Y])
        for (int i=0;i<v.ntot();i++) {
          const vec here = v.loc(Ey,i);
          inveps[Ey][Y][i] = 2.0/(feps(here+hdy)+feps(here-hdy));
        }
    } else {
      abort("Unsupported symmetry!\n");
    }
  // Allocate the conductivity arrays:
  FOR_DIRECTIONS(d) FOR_COMPONENTS(c) C[d][c] = NULL;
  FOR_DIRECTIONS(d) FOR_DIRECTIONS(d2) FOR_COMPONENTS(c) Cdecay[d][c][d2] = NULL;
}

double mat::max_eps() const {
  double themax = 0.0;
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      themax = max(themax,chunks[i]->max_eps());
  return max_to_all(themax);
}

double fields::max_eps() const {
  double themax = 0.0;
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      themax = max(themax,chunks[i]->ma->max_eps());
  return max_to_all(themax);
}

double mat_chunk::max_eps() const {
  double themax = 0.0;
  for (int i=0;i<v.ntot();i++) themax = max(themax,eps[i]);
  return themax;
}
