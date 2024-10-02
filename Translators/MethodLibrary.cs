// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

namespace Breezy.Translators
{
	class MethodLibrary
	{
		public static string AddInclude(string source, string include)
		{
			include = "#include <" + include + ">";

			if (!source.Contains(include))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = include + "\n\n" + source;
				}
				else
				{
					source = include + "\n" + source;
				}
			}

			return source;
		}

		public static string AddMethods(string source, string methods)
		{
			if (methods.Length > 0)
			{
				// Find the last #include directive.
				int includeEndIndex = source.LastIndexOf("#include");

				if (includeEndIndex != -1)
				{
					// Move to the end of the last #include line.
					includeEndIndex = source.IndexOf('\n', includeEndIndex) + 1;

					// Search for the last empty line after the last #include.
					int lastEmptyLineIndex = includeEndIndex;
					while (lastEmptyLineIndex < source.Length)
					{
						// Find the next newline.
						int nextLineIndex = source.IndexOf('\n', lastEmptyLineIndex) + 1;

						// If this line is empty or contains only whitespace, update lastEmptyLineIndex.
						if (nextLineIndex > 0 && string.IsNullOrWhiteSpace(source.Substring(lastEmptyLineIndex, nextLineIndex - lastEmptyLineIndex)))
						{
							lastEmptyLineIndex = nextLineIndex;
						}
						else
						{
							// Stop at the first non-empty line.
							break;
						}
					}

					// Insert the header after the last empty line.
					source = source.Insert(lastEmptyLineIndex, methods.ToString());
				}
				else
				{
					// No #include statements found, just prepend the header and maintain line count.
					source = methods + source;
				}
			}

			return source;
		}
	}
}
