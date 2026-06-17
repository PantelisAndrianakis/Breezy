# XML

`Xml` is a **static namespace** for reading XML. `Xml.parse(text)` turns an XML
string into a tree of `XmlNode` values; you navigate that tree with the ordinary
[collection combinators](../types/collections.md) and [closures](../types/functions.md),
so there is no separate query language to learn. Reading is the whole story here —
the reader does not write XML.

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

## v1 boundaries

- **Read-only.** There is no XML writer yet.
- **No DTD, no custom entities, no namespace-URI resolution.**
- **No XPath** — closures and the collection combinators are the query language.
- **Whole document in memory** — there is no streaming/pull cursor.
- **`Xml.parse(string)` only** — read the text yourself (e.g.
  [`File.readText`](file.md)) and pass the string.

A parsed tree is an ordinary managed value: it is reference-counted and reclaimed
when its last reference goes away, and the cycle collector traces it like any
object.

---

← [Back to the guide](../guide.md) · [Generic collections](../types/collections.md) · [First-class functions](../types/functions.md)
