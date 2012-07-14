"""
Builds a (pre)release build of SumatraPDF, including the installer,
and optionally uploads it to s3.
"""

import os
import os.path
import shutil
import sys
import time
import re

from util import log, run_cmd_throw, test_for_flag, s3UploadFilePublic
from util import s3UploadDataPublic, ensure_s3_doesnt_exist, ensure_path_exists
from util import zip_file, extract_sumatra_version, verify_started_in_right_directory
from util import build_installer_data, parse_svninfo_out, s3List, s3Delete

args = sys.argv[1:]
upload               = test_for_flag(args, "-upload")
upload_tmp           = test_for_flag(args, "-uploadtmp")
testing              = test_for_flag(args, "-test") or test_for_flag(args, "-testing")
build_test_installer = test_for_flag(args, "-test-installer") or test_for_flag(args, "-testinst") or test_for_flag(args, "-testinstaller")
build_rel_installer  = test_for_flag(args, "-testrelinst")
build_prerelease     = test_for_flag(args, "-prerelease")
svn_revision         = test_for_flag(args, "-svn-revision", True)
target_platform      = test_for_flag(args, "-platform", True)

def usage():
  print("build-release.py [-upload][-uploadtmp][-test][-test-installer][-prerelease][-platform=X64]")
  sys.exit(1)

# Terms:
#  static build  - SumatraPDF.exe single executable with mupdf code statically
#                  linked in
#  library build - SumatraPDF.exe executable that uses libmupdf.dll

# Building release version:
#   * extract version from Version.h
#   * build with nmake, sending version as argument
#   * build an installer
#   * upload to s3 kjkpub bucket. Uploaded files:
#       sumatrapdf/rel/SumatraPDF-<ver>.exe
#          uncompressed portable executable, for archival
#       sumatrapdf/rel/SumatraPDF-<ver>.pdb.zip
#          pdb symbols for libmupdf.dll, and Sumatra's static and library builds
#       sumatrapdf/rel/SumatraPDF-<ver>-install.exe
#          installer for library build
#
#   * file sumatrapdf/sumpdf-latest.txt must be manually updated

# Building pre-release version:
#   * get svn version
#   * build with nmake, sending svn version as argument
#   * build an installer
#   * upload to s3 kjkpub bucket. Uploaded files:
#       sumatrapdf/prerel/SumatraPDF-prerelease-<svnrev>.exe
#          static, portable executable
#       sumatrapdf/prerel/SumatraPDF-prerelease-<svnrev>.pdb.zip
#          pdb symbols for libmupdf.dll and Sumatra's static and library builds
#       sumatrapdf/prerel/SumatraPDF-prerelease-<svnrev>-install.exe
#          installer for library build
#       sumatrapdf/sumatralatest.js
#       sumatrapdf/sumpdf-prerelease-latest.txt

def copy_to_dst_dir(src_path, dst_dir):
  name_in_obj_rel = os.path.basename(src_path)
  dst_path = os.path.join(dst_dir, name_in_obj_rel)
  shutil.copy(src_path, dst_path)

# delete all but the last 3 pre-release builds in order to use less s3 storage
def deleteOldPreReleaseBuilds():
  s3Dir = "sumatrapdf/prerel/"
  keys = s3List(s3Dir)
  files_by_ver = {}
  for k in keys:
    #print(k.name)
    # sumatrapdf/prerel/SumatraPDF-prerelease-4819.pdb.zip
    ver = re.findall(r'sumatrapdf/prerel/SumatraPDF-prerelease-(\d+)*', k.name)
    ver = int(ver[0])
    #print(ver)
    val = files_by_ver.get(ver, [])
    #print(val)
    val.append(k.name)
    #print(val)
    files_by_ver[ver] = val
  versions = files_by_ver.keys()
  versions.sort()
  #print(versions)
  todelete = versions[:-3]
  #print(todelete)
  for vertodelete in todelete:
    for f in files_by_ver[vertodelete]:
      #print("Deleting %s" % f)
      s3Delete(f)

def sign(file_path, cert_pwd):
  # the sign tool is finicky, so copy it and cert to the same dir as
  # exe we're signing
  file_dir = os.path.dirname(file_path)
  file_name = os.path.basename(file_path)
  cert_src = os.path.join("scripts", "cert.pfx")
  sign_tool_src = os.path.join("bin", "ksigncmd.exe")
  cert_dest = os.path.join(file_dir, "cert.pfx")
  sign_tool_dest = os.path.join(file_dir, "ksigncmd.exe")
  if not os.path.exists(cert_dest): shutil.copy(cert_src, cert_dest)
  if not os.path.exists(sign_tool_dest): shutil.copy(sign_tool_src, sign_tool_dest)
  curr_dir = os.getcwd()
  os.chdir(file_dir)
  run_cmd_throw("ksigncmd.exe", "/f", "cert.pfx", "/p", cert_pwd, file_name)  
  os.chdir(curr_dir)

def main():
  global upload
  if len(args) != 0:
    usage()
  verify_started_in_right_directory()

  if build_prerelease:
    if svn_revision is None:
      run_cmd_throw("svn", "update")
      (out, err) = run_cmd_throw("svn", "info")
      ver = str(parse_svninfo_out(out))
    else:
      # allow to pass in an SVN revision, in case SVN itself isn't available
      ver = svn_revision
  else:
    ver = extract_sumatra_version(os.path.join("src", "Version.h"))
  log("Version: '%s'" % ver)

  filename_base = "SumatraPDF-%s" % ver
  if build_prerelease:
    filename_base = "SumatraPDF-prerelease-%s" % ver

  s3_dir = "sumatrapdf/rel"
  if build_prerelease:
    s3_dir = "sumatrapdf/prerel"
  if upload_tmp:
    upload = True
    s3_dir += "tmp"

  if upload:
    log("Will upload to s3 at %s" % s3_dir)

  s3_prefix = "%s/%s" % (s3_dir, filename_base)
  s3_exe           = s3_prefix + ".exe"
  s3_installer     = s3_prefix + "-install.exe"
  s3_pdb_zip       = s3_prefix + ".pdb.zip"
  s3_exe_zip       = s3_prefix + ".zip"

  s3_files = [s3_exe, s3_installer, s3_pdb_zip]
  if not build_prerelease:
    s3_files.append(s3_exe_zip)

  cert_pwd = None
  cert_path = os.path.join("scripts", "cert.pfx")
  if upload:
    map(ensure_s3_doesnt_exist, s3_files)
    if not os.path.exists(os.path.join("scripts", "cert.pfx")):
      print("scripts/cert.pfx missing")
      sys.exit(1)
    import awscreds
    cert_pwd = awscreds.certpwd

  obj_dir = "obj-rel"
  if target_platform == "X64":
    obj_dir += "64"

  if not testing and not build_test_installer and not build_rel_installer:
    shutil.rmtree(obj_dir, ignore_errors=True)

  config = "CFG=rel"
  if build_test_installer and not build_prerelease:
    obj_dir = "obj-dbg"
    config = "CFG=dbg"
  extcflags = ""
  if build_prerelease:
    extcflags = "EXTCFLAGS=-DSVN_PRE_RELEASE_VER=%s" % ver
  platform = "PLATFORM=%s" % (target_platform or "X86")

  run_cmd_throw("nmake", "-f", "makefile.msvc", config, extcflags, platform, "all_sumatrapdf")
  exe = os.path.join(obj_dir, "SumatraPDF.exe")
  if upload:
    sign(exe, cert_pwd)
    sign(os.path.join(obj_dir, "uninstall.exe"), cert_pwd)

  build_installer_data(obj_dir)
  run_cmd_throw("nmake", "-f", "makefile.msvc", "Installer", config, platform, extcflags)

  if build_test_installer or build_rel_installer:
    sys.exit(0)

  installer = os.path.join(obj_dir, "Installer.exe")
  if upload:
    sign(installer, cert_pwd)

  pdb_zip = os.path.join(obj_dir, "%s.pdb.zip" % filename_base)

  zip_file(pdb_zip, os.path.join(obj_dir, "libmupdf.pdb"))
  zip_file(pdb_zip, os.path.join(obj_dir, "Installer.pdb"), append=True)
  zip_file(pdb_zip, os.path.join(obj_dir, "SumatraPDF-no-MuPDF.pdb"), append=True)
  zip_file(pdb_zip, os.path.join(obj_dir, "SumatraPDF.pdb"), append=True)

  builds_dir = os.path.join("builds", ver)
  if os.path.exists(builds_dir):
    shutil.rmtree(builds_dir)
  os.makedirs(builds_dir)

  copy_to_dst_dir(exe, builds_dir)
  copy_to_dst_dir(installer, builds_dir)
  copy_to_dst_dir(pdb_zip, builds_dir)

  if not build_prerelease:
    exe_zip = os.path.join(obj_dir, "%s.zip" % filename_base)
    zip_file(exe_zip, exe, "SumatraPDF.exe", compress=True)
    ensure_path_exists(exe_zip)
    copy_to_dst_dir(exe_zip, builds_dir)

  if not upload: return

  if build_prerelease:
    jstxt  = 'var sumLatestVer = %s;\n' % ver
    jstxt += 'var sumBuiltOn = "%s";\n' % time.strftime("%Y-%m-%d")
    jstxt += 'var sumLatestName = "%s";\n' % s3_exe.split("/")[-1]
    jstxt += 'var sumLatestExe = "http://kjkpub.s3.amazonaws.com/%s";\n' % s3_exe
    jstxt += 'var sumLatestPdb = "http://kjkpub.s3.amazonaws.com/%s";\n' % s3_pdb_zip
    jstxt += 'var sumLatestInstaller = "http://kjkpub.s3.amazonaws.com/%s";\n' % s3_installer

  s3UploadFilePublic(installer, s3_installer)
  s3UploadFilePublic(pdb_zip, s3_pdb_zip)
  s3UploadFilePublic(exe, s3_exe)

  if build_prerelease:
    s3UploadDataPublic(jstxt, "sumatrapdf/sumatralatest.js")
    txt = "%s\n" % ver
    s3UploadDataPublic(txt, "sumatrapdf/sumpdf-prerelease-latest.txt")
    deleteOldPreReleaseBuilds()
  else:
    s3UploadFilePublic(exe_zip, s3_exe_zip)

  # Note: for release builds, must update sumatrapdf/sumpdf-latest.txt in s3
  # manually to: "%s\n" % ver

if __name__ == "__main__":
  main()
