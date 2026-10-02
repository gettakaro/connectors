# Replaces the stock join/quit block of monitor_players() with player-tracking.fragment.
# Fails unless the upstream block matches exactly once.
use strict;
my ($wrapper, $fragment) = @ARGV;
local $/;
open(my $w, '<', $wrapper) or die "read $wrapper: $!";
my $src = <$w>;
close $w;
open(my $f, '<', $fragment) or die "read $fragment: $!";
my $new = <$f>;
close $f;
my $n = () = $src =~ /^[ \t]*if \[\[ "\$line" == \*"LogNet: Join succeeded:"\* \]\]; then\n.*?\n(?=[ \t]*done < <\(tail)/msg;
die "expected 1 player-tracking block, found $n\n" unless $n == 1;
$src =~ s/^[ \t]*if \[\[ "\$line" == \*"LogNet: Join succeeded:"\* \]\]; then\n.*?\n(?=[ \t]*done < <\(tail)/$new/ms;
open($w, '>', $wrapper) or die "write $wrapper: $!";
print $w $src;
close $w;
