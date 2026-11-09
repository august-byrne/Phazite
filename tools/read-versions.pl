#!/usr/bin/perl
# SPDX-License-Identifier: AGPL-3.0-or-later
# Reads packs of a Phazite project's history (Versions/pieces/*.pack) as another tool would, from
# docs/versions-format.md alone, with nothing of Phazite's: it checks each pack's marks and index, the index against the
# pack's name (when the file is named by a SHA-256), and every piece against its own name, then lists the pieces, a line
# each, in the order of their names: the name, the size, and what the piece holds (its text, or "base64:" and its bytes).
# Exits with 1, saying why on stderr, if anything doesn't check out.
#
#   perl tools/read-versions.pl Versions/pieces/*.pack
#
# CI lists the test vectors with it and compares the list with tests/golden/format/song.txt, which Phazite's own reader
# writes (tests/GoldenFormatTests.cpp), so that this reader and the specification stay true to what Phazite writes.
use strict;
use warnings;
use Digest::SHA qw(sha256_hex);
use IO::Uncompress::Inflate qw(inflate $InflateError);
use MIME::Base64 qw(encode_base64);

my $mark = 'PZPK';
my $entrySize = 32 + 8 + 4 + 4; # a piece's name, where it starts, its sizes compressed and not
my $failed = 0;
my %pieces;

sub fail
{
    print STDERR "@_\n";
    $failed = 1;
}

for my $path (@ARGV)
{
    open (my $fh, '<:raw', $path) or do { fail ("$path: can't be read: $!"); next; };
    local $/;
    my $bytes = <$fh>;
    close $fh;

    my $length = length $bytes;

    if ($length < 4 + 16 || substr ($bytes, 0, 4) ne $mark || substr ($bytes, $length - 4, 4) ne $mark)
    {
        fail ("$path: not a whole pack (its marks)");
        next;
    }

    my $count = unpack ('V', substr ($bytes, $length - 16, 4));
    my ($low, $high) = unpack ('VV', substr ($bytes, $length - 12, 8));
    my $indexStart = $low + $high * 4294967296;

    if ($indexStart < 4 || $indexStart + $count * $entrySize != $length - 16)
    {
        fail ("$path: its index doesn't fit");
        next;
    }

    my $index = substr ($bytes, $indexStart, $count * $entrySize);
    (my $file = $path) =~ s{.*[/\\]}{};

    if ($file =~ /^([0-9a-f]{64})\.pack$/ && $1 ne sha256_hex ($index))
    {
        fail ("$path: its index doesn't hash to its name");
        next;
    }

    for my $i (0 .. $count - 1)
    {
        my $entry = substr ($index, $i * $entrySize, $entrySize);
        my $name = unpack ('H64', substr ($entry, 0, 32));
        my ($startLow, $startHigh, $compressedSize, $size) = unpack ('VVVV', substr ($entry, 32, 16));
        my $start = $startLow + $startHigh * 4294967296;

        if ($start < 4 || $start + $compressedSize > $indexStart || $size > $compressedSize * 1032 + 1024)
        {
            fail ("$path: piece $name doesn't fit");
            next;
        }

        my $compressed = substr ($bytes, $start, $compressedSize);
        my $piece;

        if (! inflate (\$compressed => \$piece) || length ($piece) != $size || sha256_hex ($piece) ne $name)
        {
            fail ("$path: piece $name isn't what its name says");
            next;
        }

        $pieces{$name} = $piece;
    }
}

for my $name (sort keys %pieces)
{
    my $piece = $pieces{$name};
    my $what = substr ($piece, 0, 1) eq '<' ? $piece : 'base64:' . encode_base64 ($piece, '');
    print "$name " . length ($piece) . " $what\n";
}

exit $failed;
