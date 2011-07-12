#!/bin/sh

REPOS="http://serf.googlecode.com/svn/"

if test $# != 2; then
  echo "USAGE: $0 TAG APR-SOURCE-PARENT"
  exit 1
fi

version=$1

# Convert to absolute path.
srcdir=`(cd $2 ; pwd)`

# provide for examining dist.sh output before creating a tag
if test "${version}" = "trunk"; then
  url="${REPOS}/trunk"
else
  url="${REPOS}/tags/${version}"
fi

release="serf-${version}"

# on Mac OS, TMPDIR is scary long. we want an unexpanded form in $short
work="${TMPDIR-/tmp}/serf-dist.$$"
short='${TMPDIR}'/serf-dist.$$

echo "Preparing ${release} in ${short} ..."

mkdir "${work}"
cd "${work}"

echo "Exporting latest serf ..."
svn export --quiet "${url}" "${release}" || exit 1
echo "`find ${release} -type f | wc -l` files exported"

prepare_directory()
{
cd "${release}"

echo "Running buildconf ..."
if ! ./buildconf --with-apr="${srcdir}/apr" --with-apr-util="${srcdir}/apr-util" ; then
  echo "Exiting..."
  exit 1
fi

# Remove anything that should not be in the distribution
echo "Removing from release: dist.sh"
rm dist.sh

major="`sed -n '/SERF_MAJOR_VERSION/s/[^0-9]*//p' serf.h`"
minor="`sed -n '/SERF_MINOR_VERSION/s/[^0-9]*//p' serf.h`"
patch="`sed -n '/SERF_PATCH_VERSION/s/[^0-9]*//p' serf.h`"

actual_version="${major}.${minor}.${patch}"

cd "${work}"

if test "${version}" != "trunk" -a "${version}" != "${actual_version}"; then
  echo "ERROR: exported version does not match"
  exit 1
fi

}

prepare_directory

tarball="${work}/${release}.tar"
tar -cf "${tarball}" "${release}"

bzip2 "${tarball}"
echo "${short}/${release}.tar.bz2 ready."

# Let's redo everything for a Windows .zip file
echo "Saving ${release} as ${release}.unix"
mv "${release}" "${release}.unix"

echo "Exporting latest serf using CRLF ..."
svn export --native-eol=CRLF --quiet "${url}" "${release}" || exit 1
echo "`find ${release} -type f | wc -l` files exported"

### generated files have wrong line-ending. is that an issue?
prepare_directory

if ! diff -brq "${release}.unix" "${release}"; then
  echo "ERROR: export directories differ."
  exit 1
fi

zipfile="${work}/${release}.zip"
zip -9rq "${zipfile}" "${release}"
echo "${short}/${release}.zip ready."
