﻿// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

namespace Breezy.Translators
{
	public class BaseLibrary
	{
		public static string AddHeader(string source, string cppHeader)
		{
			if (cppHeader.Length > 0)
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

					// Insert the cppHeader after the last empty line.
					source = source.Insert(lastEmptyLineIndex, cppHeader.ToString());
				}
				else
				{
					// No #include statements found, just prepend the header and maintain line count.
					source = cppHeader + source;
				}
			}

			return source;
		}
	}
}
