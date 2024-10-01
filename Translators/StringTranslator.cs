// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class StringTranslator
	{
		public static string Translate(string source)
		{
			// Use regex to replace only "string" as a type declaration.
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			source = Regex.Replace(source, stringDeclarationPattern, "std::string $1");

			return source;
		}
	}
}
