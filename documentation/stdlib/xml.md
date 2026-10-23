# XML

`Xml` is a **static namespace** for reading and writing XML. `Xml.parse(text)` turns an
XML string into a tree of `XmlNode` values; you navigate that tree with the ordinary
[collection combinators](../types/collections.md) and [closures](../types/functions.md),
so there is no separate query language to learn. `Xml.stringify(node)` writes a tree back
out, and a parsed document comes back **exactly as it was written** (comments, formatting
and all) except where you changed it. To build output in one pass, see
[XmlWriter](#streaming-output-xmlwriter).

← [Back to the guide](../guide.md)

---

## Parsing

```breezy
string text = File.readText("catalog.xml");   // Get the bytes however you like.
XmlNode root = Xml.parse(text);                // The root element.
print(root.name);                              // e.g. "catalog".
```

`Xml.parse` returns the **root element**. Malformed input **throws a catchable
`XmlException`** whose message carries the line and column:

```breezy
try
{
	XmlNode r = Xml.parse("<a><b></a>");       // Mismatched end tag.
}
catch (XmlException e)
{
	print(e.message);    // XML parse error at line 1, column 11: Mismatched end tag.
}
```

---

## The node

An `XmlNode` exposes its tag name, its direct text, its attributes, and its child
elements.

| Member | Type | Meaning |
| --- | --- | --- |
| `node.name` | `string` | The tag name. A prefixed name is kept literal (`"svg:rect"`). |
| `node.text` | `string` | This element's **direct** text, with entities decoded and CDATA included; text inside child elements is not included. |
| `node.attr(name)` | `string` | The attribute value, or `""` when the attribute is absent. |
| `node.hasAttr(name)` | `bool` | Whether the attribute is present. |
| `node.attrCount` | `int` | The number of attributes. |
| `node.attrNameAt(i)` | `string` | The attribute name at index `i` (to enumerate them). |
| `node.children` | `List<XmlNode>` | The direct child elements. |
| `node.descendants()` | `List<XmlNode>` | Every descendant element, pre-order (the node itself excluded). |

```breezy
XmlNode root = Xml.parse("<box w='10' h='20'><label>hi</label></box>");
print(root.attr("w"));        // 10.
print(root.hasAttr("z"));     // false.
print(root.children.size);    // 1.
print(root.children.get(0).text);   // hi.
```

---

## Traversal is just collections + closures

Because `children` and `descendants()` are ordinary `List<XmlNode>` values, you
query a document with `forEach` / `filter` / `reduce` and lambdas — the same tools
you use on any list:

```breezy
XmlNode catalog = Xml.parse(text);

// Print each book's title.
catalog.children.forEach(book => print(book.attr("title")));

// The books priced over 30.
List<XmlNode> pricey = catalog.children.filter(b => b.attr("price").toInt() > 30);

// Sum every price anywhere in the document.
int total = catalog.descendants()
				   .filter(n => n.name.equals("book"))
				   .reduce(0, (acc, b) => acc + b.attr("price").toInt());
```

`children`/`descendants()` searches stay shallow vs deep by your choice:
`children` is the direct children only; `descendants()` is the whole subtree.

---

## The supported subset

The reader covers the XML most data and config documents use:

- **Elements** — `<tag>…</tag>` and self-closing `<tag/>`, nested to any depth.
- **Attributes** — `name="value"` and `name='value'` (both quote styles).
- **Text** — character data between tags, whitespace preserved as written.
- **Entities** — the five built-ins (`&lt; &gt; &amp; &quot; &apos;`) and numeric
  character references (`&#65;`, `&#x41;`), decoded in text and in attribute values.
- **CDATA** — `<![CDATA[ … ]]>` contents are taken literally.
- **Skipped** — comments `<!-- -->`, the `<?xml ?>` declaration, and other
  processing instructions are consumed and ignored.

Namespaces are **literal**: `xmlns`/`xmlns:foo` are ordinary attributes and a
prefixed name like `foo:bar` is just the `name` string — no URI resolution.

A document that is not well formed throws `XmlException`. So does a **DOCTYPE/DTD**
(`<!DOCTYPE …>`) and an **unknown entity** (`&foo;` outside the built-in set).

---

## Writing

### Lossless round trip

`Xml.stringify(node)` returns XML text. For a parsed document, everything you did not
change comes back byte-for-byte: comments, the `<?xml ?>` declaration, processing
instructions, CDATA, whitespace and indentation, attribute order, spacing and quote
style, entity spellings and line endings. Edit a value in a hand-maintained file and
only that value changes:

```breezy
XmlNode root = Xml.parse(File.readText("items.xml"));
root.descendants()
	.filter(n => n.name.equals("item") && n.attr("id").equals("57"))
	.forEach(n => n.setAttr("price", "120"));
File.writeText("items.xml", Xml.stringify(root));
```

```xml
<items>
	<!-- prices in gold -->
	<item id="57" price="120"/>
	<item id="58" price='7'/>
</items>
```

`Xml.stringify(node, true)` also puts `<?xml version="1.0" encoding="UTF-8"?>` in front
when the output does not already start with a declaration.

### Editing

| Call | Effect |
| --- | --- |
| `node.setAttr(name, value)` | Adds the attribute, or changes it in place (same position, spacing and quotes). |
| `node.removeAttr(name)` | Removes the attribute and the space before it. |
| `node.setText(text)` | Replaces a leaf element's content. |
| `node.addChild(c)` / `node.insertChild(i, c)` | Adds a child at the end / at index `i`. |
| `node.removeChild(c)` | Removes a child, with the comment and indentation in front of it. |
| `node.rename(name)` | Renames the element in both tags. |
| `Xml.element(name)` | A new, empty element. |

Editing `node.children` directly (`add`, `removeAt`, ...) works the same way.

New content follows the document's own style: a new child copies the indentation of its
siblings, nested content goes one indentation step deeper, and added lines use the
document's line ending. A tree built only from `Xml.element` is written compact:

```breezy
XmlNode doc = Xml.element("doc");
doc.setAttr("v", "1");
XmlNode c = Xml.element("c");
c.setText("hi");
doc.addChild(c);
print(Xml.stringify(doc));   // <doc v="1"><c>hi</c></doc>
```

Text is escaped (`&` `<` `>`), and so are attribute values (`&` `<` and the quote).

### Streaming output: XmlWriter

`XmlWriter` writes XML in one pass, without building a tree:

```breezy
XmlWriter w = new XmlWriter();
w.start("list");
w.attr("n", "2");
w.start("item");
w.text("x < y");
w.end();
w.end();
print(w.toString());   // <list n="2"><item>x &lt; y</item></list>
```

| Call | Effect |
| --- | --- |
| `start(name)` | Opens an element. |
| `attr(name, value)` | Adds an attribute; only right after `start`. |
| `text(t)` | Escaped text. |
| `comment(t)` | `<!--t-->`. |
| `end()` | Closes the innermost element (`<x/>` when it is empty). |
| `toString()` | The XML so far. |

### Errors

Writing mistakes throw `XmlException`: an invalid element or attribute name, a control
character XML does not allow (anything below `0x20` except tab, line feed and carriage
return), `setText` on an element that has child elements, adding a node inside itself
(directly or through `children`), an index out of range, removing a node that is not a
child, a comment containing `--`, `attr` after content, `end` with nothing open, a second
root element, and `toString` while an element is still open.

---

## v1 boundaries

- **No reformatting.** `Xml.stringify` keeps a document's own layout; there is no
  pretty-printer.
- **Documents under 2 GiB.**
- **No DTD, no custom entities, no namespace-URI resolution.**
- **No XPath** — closures and the collection combinators are the query language.
- **Whole document in memory** — there is no streaming/pull cursor.
- **`Xml.parse(string)` only** — read the text yourself (e.g.
  [`File.readText`](file.md)) and pass the string.

A parsed tree is an ordinary managed value: it is reference-counted and reclaimed
when its last reference goes away, and the cycle collector traces it like any
object. Element and attribute names are stored once per document and shared by
every node that uses them, so a document of repeated records costs little beyond
its values, and a leaf element gets its `children` list only when a program reads it.

---

← [Back to the guide](../guide.md) · [JSON](json.md) · [Generic collections](../types/collections.md) · [First-class functions](../types/functions.md)
