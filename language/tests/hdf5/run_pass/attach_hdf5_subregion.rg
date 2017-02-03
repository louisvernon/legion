-- Copyright 2017 Stanford University
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

import "regent"

local c = terralib.includec("assert.h")

local hdf5 = terralib.includec("hdf5.h")
-- there's some funny business in hdf5.h that prevents terra from being able to
--  see some of the #define's, so we fix it here, and hope the HDF5 folks don't
--  change the internals very often...
hdf5.H5F_ACC_TRUNC = 2
hdf5.H5T_STD_I32LE = hdf5.H5T_STD_I32LE_g
hdf5.H5T_STD_I64LE = hdf5.H5T_STD_I64LE_g
hdf5.H5T_IEEE_F64LE = hdf5.H5T_IEEE_F64LE_g
hdf5.H5P_DEFAULT = 0

fspace t {
  a : int32,
  b : int64,
  c : double,
}

local filename = os.tmpname() .. ".hdf"

terra generate_hdf5_file(filename : rawstring)
  var fid = hdf5.H5Fcreate(filename, hdf5.H5F_ACC_TRUNC, hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT)
  --c.assert(fid > 0)

  var dims : hdf5.hsize_t[3]
  dims[0] = 4
  dims[1] = 4
  dims[2] = 4
  var did = hdf5.H5Screate_simple(3, dims, [&uint64](0))
  --c.assert(did > 0)

  var ds1id = hdf5.H5Dcreate2(fid, "a", hdf5.H5T_STD_I32LE, did,
                              hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT)
  --c.assert(ds1id > 0)
  hdf5.H5Dclose(ds1id)

  var ds2id = hdf5.H5Dcreate2(fid, "b", hdf5.H5T_STD_I64LE, did,
                              hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT)
  --c.assert(ds2id > 0)
  hdf5.H5Dclose(ds2id)

  var ds3id = hdf5.H5Dcreate2(fid, "c", hdf5.H5T_IEEE_F64LE, did,
                              hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT, hdf5.H5P_DEFAULT)
  --c.assert(ds3id > 0)
  hdf5.H5Dclose(ds3id)

  hdf5.H5Sclose(did)
  hdf5.H5Fclose(fid)
end

task fill_region(r : region(ispace(int3d), t), seed : int32)
where writes(r.{a,b,c}) do
  for p in r do
    r[p].a = 1000 * seed + p.x + 10 * p.y + 100 * p.z
    r[p].b = 1000 * seed + 5 * p.x + 50 * p.y + 500 * p.z
    r[p].c = 1000 * seed + 0.1 * p.x + 0.01 * p.y + 0.001 * p.z
  end
end

task compare_regions(is : ispace(int3d), r1 : region(is, t), r2 : region(ispace(int3d), t)) : int
where reads(r1.{a,b,c}), reads(r2.{a,b,c}) do
  var errors = 0
  for p in is do
    if(r1[p].a ~= r2[p].a) then
      errors += 1
      regentlib.c.printf("[%d,%d,%d]: a mismatch - %d %d\n", p.x, p.y, p.z, r1[p].a, r2[p].a)
    else
      regentlib.c.printf("[%d,%d,%d]: %d\n", p.x, p.y, p.z, r1[p].a)
    end
  end
  return errors
end

task main()
  var is = ispace(int3d, {4, 4, 4})
  var r1 = region(is, t)
  var r2 = region(is, t)

  var cs = ispace(int3d, {4, 1, 1})
  var p1 = partition(equal, r1, cs)
  var p2 = partition(equal, r2, cs)

  generate_hdf5_file(filename)

  -- test 1: attach in read-only mode and acquire/release
  --  (should make a local copy)
  if true then
    regentlib.c.printf("test 1\n")
    fill_region(r1, 2)
    -- fill_region(r2, 3)
    for x in r2 do x.{a, b, c} = 1 end -- force an inline mapping

    attach(hdf5, r2.{a, b, c}, filename, regentlib.file_read_write)
    acquire(r2)
    fill_region(r2, 3)
    release(r2)
    -- for c in cs do
    --   acquire((p2[c]))
    --   -- copy(r2.a, r1.a)
    --   -- copy(r2.b, r1.b)
    --   -- copy(r2.c, r1.c)
    --   compare_regions(p1[c].ispace, p1[c], p2[c])
    --   release((p2[c]))
    -- end
    detach(hdf5, r2.{a, b, c})

    var errors = 0
    attach(hdf5, r2.{a, b, c}, filename, regentlib.file_read_write)
    for c in cs do
      acquire((p2[c]))
      -- copy(r2.a, r1.a)
      -- copy(r2.b, r1.b)
      -- copy(r2.c, r1.c)
      errors += compare_regions(p1[c].ispace, p1[c], p2[c])
      release((p2[c]))
    end
    regentlib.assert(errors == 0, "errors detected")
    detach(hdf5, r2.{a, b, c})

  end
end

regentlib.start(main)