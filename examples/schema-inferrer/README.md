# Schema inferrer

Infers an XSD schema from sample XML files - a practical port of the .NET
`XmlSchemaInference` tool, written entirely in Breezy.

It reads each document into Breezy's XML DOM, merges the observed structure
across every instance (elements, attributes, occurrence, and simple types), and
emits a valid schema.

## Run

```
breezy examples/schema-inferrer Main      # Demo: prints the XSD for a built-in sample.
```

As a tool, the first argument is the output path and the rest are input files:

```
Main out.xsd catalog1.xml catalog2.xml    # Merges both, writes out.xsd.
```

## What it infers

- **Attributes** - `use="required"` when present on every instance of the tag,
  otherwise `optional`.
- **Children** - `minOccurs="0"` when some instance lacked the child,
  `maxOccurs="unbounded"` when any instance had more than one.
- **Simple types** - value-based and widening: `boolean` < `int` < `decimal`,
  with anything mixed or non-numeric becoming `xs:string`.

Types are keyed by tag name and merged globally, which suits regular,
data-style XML. See `docs/superpowers/specs/2026-06-24-xsd-inference-design.md`
for the full design and the simplifications relative to .NET.
