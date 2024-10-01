// Author: Pantelis Andrianakis
// Creation Date: October 2nd 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class CollectionTranslator : BaseLibrary
	{
		public static string Process(string source)
		{
			bool foundVector = false;
			bool foundMap = false;

			// Define regex patterns for List<T> and Map<K, V>.
			string listPattern = @"List<([^\>]+)>"; // Matches List<T>.
			string mapPattern = @"Map<([^\s,]+)\s*,\s*([^\>]+)>"; // Matches Map<K, V>.

			// Replace 'List<T>' with 'std::vector<T>'.
			source = Regex.Replace(source, listPattern, match =>
			{
				foundVector = true;
				string type = match.Groups[1].Value; // Capture the type inside List<>.
				return $"std::vector<{type}>";
			});

			// Replace 'Map<K, V>' with 'std::unordered_map<K, V>'.
			source = Regex.Replace(source, mapPattern, match =>
			{
				foundMap = true;
				string keyType = match.Groups[1].Value; // Capture key type inside Map<K, V>.
				string valueType = match.Groups[2].Value; // Capture value type inside Map<K, V>.
				return $"std::unordered_map<{keyType}, {valueType}>";
			});

			// Add necessary #include statements based on found elements.
			if (foundVector && !source.Contains("#include <vector>"))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = "#include <vector>\n\n" + source;
				}
				else
				{
					source = "#include <vector>\n" + source;
				}
			}
			if (foundMap && !source.Contains("#include <unordered_map>"))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = "#include <unordered_map>\n\n" + source;
				}
				else
				{
					source = "#include <unordered_map>\n" + source;
				}
			}

			// Return the modified source.
			return source;
		}
	}
}
