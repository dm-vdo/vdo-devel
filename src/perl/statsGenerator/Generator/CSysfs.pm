##
# C file generator for statistics sysfs glue.
#
# @synopsis
#
#     use Generator::CSysfs
#
#
# @description
#
# C<Generator::CSysfs> generates C source files for the statistics
# sysfs directory from a parsed .stats file.
#
# $Id$
##
package Generator::CSysfs;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertFalse
  assertMinArgs
  assertNotDefined
  assertNumArgs
  assertTrue
);

use base qw(Generator);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple List of the attr structs emitted by the Generator
     attrStructs => [],
     # @ple Set of entries in the sysfs directory (values are ignored)
     entries => {},
     # @ple Current type of statistic, 'kernel' or 'vdo'
     flavor => "",
     # @ple The language this generator generates
     language => 'CSysfs',
     # @ple Stack of structure names for the current field
     structs => [],
     # @ple The attribute which is the output file name for this generator
     outputFileAttribute  => 'csysfsOutput',
    );

######################################################################
# @inherit
##
sub generateHeader {
  my ($self, $group) = assertNumArgs(2, @_);

  $self->generateSPDX($group);
  $self->emit('/*');
  $self->indent(' * ');
  $self->SUPER::generateHeader($group);
  $self->undent();
  $self->emit(' */');
  $self->blankLine();

  my $headerText = <<"EOH";
#include <linux/mutex.h>

#include "logger.h"

#include "dedupe.h"
#include "pool-sysfs.h"
#include "statistics.h"
#include "vdo.h"

struct pool_stats_attribute {
	struct attribute attr;
	ssize_t (*print)(struct vdo_statistics *stats, char *buf);
};

static ssize_t pool_stats_attr_show(struct kobject *directory,
				    struct attribute *attr,
				    char *buf)
{
	ssize_t size;
	struct pool_stats_attribute *pool_stats_attr =
		container_of(attr, struct pool_stats_attribute, attr);
	struct vdo *vdo = container_of(directory, struct vdo, stats_directory);

	if (pool_stats_attr->print == NULL)
		return -EINVAL;

	mutex_lock(&vdo->stats_mutex);
	vdo_fetch_statistics(vdo, &vdo->stats_buffer);
	size = pool_stats_attr->print(&vdo->stats_buffer, buf);
	mutex_unlock(&vdo->stats_mutex);

	return size;
}

const struct sysfs_ops vdo_pool_stats_sysfs_ops = {
	.show = pool_stats_attr_show,
	.store = NULL,
};
EOH
  $self->emit($headerText);
}

######################################################################
# @inherit
##
sub emitVersion {
  my ($self, $group) = assertNumArgs(2, @_);
  # nothing to do
}

######################################################################
# @inherit
##
sub generateEnum {
  my ($self, $enum) = assertNumArgs(2, @_);
  # nothing to do
}

######################################################################
# @inherit
##
sub generateStruct {
  my ($self, $struct) = assertNumArgs(2, @_);

  if ($struct->{name} eq "VDOStatistics") {
    foreach my $field ($struct->getChildren()) {
      # There is no easy way to disable this in .stats; these
      # attributes are added by the parser.
      if ($field->{name} ne "version" && $field->{name} ne "releaseVersion") {
        $self->generate($field);
      }
    }
  }
}

######################################################################
# @inherit
##
sub generateField {
  my ($self, $field) = assertNumArgs(2, @_);

  if ($field->{parent}->{name} eq "VDOStatistics") {
    $self->{flavor} = "vdo";
    $self->{structs} = [];
  }
  assertTrue($self->{flavor} eq "vdo");

  # This code does not currently handle non-string arrays, but there
  # aren't any among the statistics we handle.
  if ($field->getType("C") ne "char") {
    assertNotDefined($field->getArraySize());
  }

  my $pct = $field->getType("kernel");
  if ($pct !~ qr/%/) {
    $self->generateCompositeField($field);
    return;
  }

  my $userPct = $field->getType("user");
  #
  # Set parameters for code geneation.
  #
  my $name           = $self->camelcaseToKernelStyle($field);
  my $sysfsFieldName = join("_", @{$self->{structs}}, $name);
  my $statsFieldName = join(".", @{$self->{structs}}, $name);
  my $attrName       = "pool_stats_attr_$sysfsFieldName";
  my $printer        = "pool_stats_print_$sysfsFieldName";
  my $structField    = "stats->$statsFieldName";

  # Blow up if this would be a duplicate sysfs entry.
  assertFalse(exists($self->{entries}{$sysfsFieldName}));
  $self->{entries}{$sysfsFieldName} = undef;

  #
  # Generate code.
  #
  $self->blankLine();
  $self->printComment($field);
  $self->emit("static ssize_t");
  $self->replaceAndEmit("PRINTER(struct vdo_statistics *stats, char *buf)",
                        "PRINTER", $printer);
  $self->emit("{");
  $self->indent();
  if ($pct =~ qr/%/) {
    $self->emit("#ifdef __KERNEL__");
    $self->replaceAndEmit(qq|return sprintf(buf, "PCT\\n", STRUCTFIELD);|,
                          "PCT", $pct, "STRUCTFIELD", $structField);
    $self->emit("#else");
    $self->replaceAndEmit(qq|return sprintf(buf, "PCT\\n", STRUCTFIELD);|,
                          "PCT", $userPct, "STRUCTFIELD", $structField);
    $self->emit("#endif // __KERNEL__");
  }
  $self->undent();
  $self->emit("}");

  $self->blankLine();
  $self->replaceAndEmit("static struct pool_stats_attribute ATTR = {", "ATTR",
                       $attrName);
  $self->indent();
  $self->replaceAndEmit(".attr = { .name = \"ENTRYNAME\", .mode = 0444, },",
                       "ENTRYNAME", $sysfsFieldName);
  $self->replaceAndEmit(".print = PRINTER,", "PRINTER", $printer);
  $self->undent();
  $self->emit("};");

  push(@{$self->{attrStructs}}, $attrName);
}

######################################################################
# Generate a composite field (i.e., a structure).
#
# @param  field          the field
##
sub generateCompositeField {
  my ($self, $field) = assertNumArgs(2, @_);

  push(@{$self->{structs}}, $self->camelcaseToKernelStyle($field));
  foreach my $subfield ($field->{type}->getChildren()) {
    if ($self->shouldSkip($subfield)) {
      next;
    }
    $self->generateField($subfield);
  }
  pop(@{$self->{structs}});
}

######################################################################
# Print a C comment.
#
# @param statistic  The statistic whose comment is to be printed
##
sub printComment {
  my ($self, $statistic) = assertNumArgs(2, @_);
  my $comment            = $statistic->getAttribute('comment');
  if (!defined($comment)) {
    return;
  }

  my @comment = split("\n", $comment);
  if (@comment == 1) {
    $self->emit("/* $comment */");
    return;
  }

  $self->emit('/*');
  $self->indent(' * ');
  map { $self->emit($_) } @comment;
  $self->undent();
  $self->emit(' */');
}

######################################################################
# @inherit
##
sub generateTrailer {
  my ($self, $statistic) = assertNumArgs(2, @_);

  $self->blankLine();
  $self->emit("struct attribute *vdo_pool_stats_attrs[] = {");
  $self->indent();
  foreach my $attr (@{$self->{attrStructs}}) {
    $self->emit("&" . $attr . ".attr,");
  }
  $self->emit("NULL,");
  $self->undent();
  $self->emit("};");
}

1;
