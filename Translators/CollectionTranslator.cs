// Author: Pantelis Andrianakis
// Creation Date: October 2nd 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class CollectionTranslator : MethodLibrary
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

			// Replace occurrences of `std::vector<string>` with `std::vector<std::string>`.
			string vectorStringPattern = @"std::vector<string>";
			source = Regex.Replace(source, vectorStringPattern, match =>
			{
				foundVector = true;
				return "std::vector<std::string>";
			});

			// Replace occurrences of `std::unordered_map<string, T>` and `std::unordered_map<T, string>` with `std::unordered_map<std::string, T>`.
			string mapStringPattern1 = @"std::unordered_map<string,\s*([^\>]+)>";
			string mapStringPattern2 = @"std::unordered_map<([^\>]+),\s*string>";
			string mapStringPattern3 = @"std::unordered_map<string,\s*string>";

			// Replace `std::unordered_map<string, T>` with `std::unordered_map<std::string, T>`.
			source = Regex.Replace(source, mapStringPattern1, match =>
			{
				foundMap = true;
				string valueType = match.Groups[1].Value; // Capture the value type.
				return $"std::unordered_map<std::string, {valueType}>";
			});

			// Replace `std::unordered_map<T, string>` with `std::unordered_map<T, std::string>`.
			source = Regex.Replace(source, mapStringPattern2, match =>
			{
				foundMap = true;
				string keyType = match.Groups[1].Value; // Capture the key type.
				return $"std::unordered_map<{keyType}, std::string>";
			});

			// Replace `std::unordered_map<string, string>` with `std::unordered_map<std::string, std::string>`.
			source = Regex.Replace(source, mapStringPattern3, match =>
			{
				foundMap = true;
				return "std::unordered_map<std::string, std::string>";
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
