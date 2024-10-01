// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class StringTranslator
	{
		public static string Translate(string sourceCode)
		{
			// Use regex to replace only "string" as a type declaration.
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			string cppCode = Regex.Replace(sourceCode, stringDeclarationPattern, "std::string $1");

			return cppCode;
		}
	}
}
