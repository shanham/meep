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

#include "meep.h"
#include "meep_internals.h"

namespace meep {

void fields::update_e_from_d() {
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      chunks[i]->update_e_from_d();
}

void fields_chunk::update_e_from_d() {
  double *d_minus_p[5][2];
  bool have_d_minus_p = false;
  DOCMP FOR_ELECTRIC_COMPONENTS(ec) {
    if (((pol || e_sources) && f[ec][cmp]) || s->kerr[ec]) {
      d_minus_p[ec][cmp] = new double[v.ntot()];
      have_d_minus_p = true;
    } else {
      d_minus_p[ec][cmp] = 0;
    }
  }
  update_e_from_d_prepare(d_minus_p, have_d_minus_p);
  update_e_from_d_sources(d_minus_p, have_d_minus_p);
  update_e_from_d_update(d_minus_p, have_d_minus_p);
  DOCMP FOR_ELECTRIC_COMPONENTS(ec) delete[] d_minus_p[ec][cmp];
}

} // namespace meep
