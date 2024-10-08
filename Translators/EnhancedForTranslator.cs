﻿// Author: Pantelis Andrianakis
// Creation Date: October 2nd 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class EnhancedForTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			// Define regex pattern to match Java-style enhanced for loops.
			string foreachPattern = @"for\s*\(\s*([^\s]+)\s*:\s*([^\)]+)\s*\)";

			// Replace 'for(x : y)' with 'for (const auto& x : y)'.
			source = Regex.Replace(source, foreachPattern, match =>
			{
				string variable = match.Groups[1].Value;
				string collection = match.Groups[2].Value;
				return $"for (const auto& {variable} : {collection})";
			});

			// Return the modified source.
			return source;
		}
	}
}
