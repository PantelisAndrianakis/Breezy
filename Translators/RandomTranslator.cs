// Author: Pantelis Andrianakis
// Creation Date: October 4th 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class RandomTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundGetIntMinMax = false;
			bool foundGetLongMinMax = false;
			bool foundGetFloatMinMax = false;
			bool foundGetDoubleMinMax = false;
			bool foundGetInt = false;
			bool foundGetLong = false;
			bool foundGetFloat = false;
			bool foundGetDouble = false;
			bool foundFillBytes = false;

			// Support for random method names to avoid conflicts.
			string randomGetIntMinMaxSuffix = "";
			string randomGetLongMinMaxSuffix = "";
			string randomGetFloatMinMaxSuffix = "";
			string randomGetDoubleMinMaxSuffix = "";
			string randomGetIntSuffix = "";
			string randomGetLongSuffix = "";
			string randomGetFloatSuffix = "";
			string randomGetDoubleSuffix = "";
			string randomFillBytesSuffix = "";

			// Define the regex patterns to find Random.getInt, Random.getLong, Random.getFloat, Random.getDouble and Random.fillBytes.
			string getIntMinMaxPattern = @"Random\.(?i)getInt\(([^;]+),\s*([^;]+)\)";
			string getLongMinMaxPattern = @"Random\.(?i)getLong\(([^;]+),\s*([^;]+)\)";
			string getFloatMinMaxPattern = @"Random\.(?i)getFloat\(([^;]+),\s*([^;]+)\)";
			string getDoubleMinMaxPattern = @"Random\.(?i)getDouble\(([^;]+),\s*([^;]+)\)";
			string getIntPattern = @"Random\.(?i)getInt\(([^;]+)\)";
			string getLongPattern = @"Random\.(?i)getLong\(([^;]+)\)";
			string getFloatPattern = @"Random\.(?i)getFloat\(([^;]+)\)";
			string getDoublePattern = @"Random\.(?i)getDouble\(([^;]+)\)";
			string fillBytesPattern = @"Random\.(?i)fillBytes\(([^;]+)\)";

			// Replace Random.getInt with randomGetIntMinMax and track if found.
			source = Regex.Replace(source, getIntMinMaxPattern, match =>
			{
				foundGetIntMinMax = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetIntMinMax("))
				{
					randomGetIntMinMaxSuffix = GetRandomMethodIdentifier();
				}
				string minParam = match.Groups[1].Value;
				string maxParam = match.Groups[2].Value;
				return $"randomGetIntMinMax{randomGetIntMinMaxSuffix}({minParam}, {maxParam})";
			});

			// Replace Random.getLong with randomGetLongMinMax and track if found.
			source = Regex.Replace(source, getLongMinMaxPattern, match =>
			{
				foundGetLongMinMax = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetLongMinMax("))
				{
					randomGetLongMinMaxSuffix = GetRandomMethodIdentifier();
				}
				string minParam = match.Groups[1].Value;
				string maxParam = match.Groups[2].Value;
				return $"randomGetLongMinMax{randomGetLongMinMaxSuffix}({minParam}, {maxParam})";
			});

			// Replace Random.getFloat with randomGetFloatMinMax and track if found.
			source = Regex.Replace(source, getFloatMinMaxPattern, match =>
			{
				foundGetFloatMinMax = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetFloatMinMax("))
				{
					randomGetFloatMinMaxSuffix = GetRandomMethodIdentifier();
				}
				string minParam = match.Groups[1].Value;
				string maxParam = match.Groups[2].Value;
				return $"randomGetFloatMinMax{randomGetFloatMinMaxSuffix}({minParam}, {maxParam})";
			});

			// Replace Random.getDouble with randomGetDoubleMinMax and track if found.
			source = Regex.Replace(source, getDoubleMinMaxPattern, match =>
			{
				foundGetDoubleMinMax = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetDoubleMinMax("))
				{
					randomGetDoubleMinMaxSuffix = GetRandomMethodIdentifier();
				}
				string minParam = match.Groups[1].Value;
				string maxParam = match.Groups[2].Value;
				return $"randomGetDoubleMinMax{randomGetDoubleMinMaxSuffix}({minParam}, {maxParam})";
			});

			// Replace Random.getInt with randomGetInt and track if found.
			source = Regex.Replace(source, getIntPattern, match =>
			{
				foundGetInt = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetInt("))
				{
					randomGetIntSuffix = GetRandomMethodIdentifier();
				}
				string parameter = match.Groups[1].Value;
				return $"randomGetInt{randomGetIntSuffix}({parameter})";
			});

			// Replace Random.getLong with randomGetLong and track if found.
			source = Regex.Replace(source, getLongPattern, match =>
			{
				foundGetLong = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetLong("))
				{
					randomGetLongSuffix = GetRandomMethodIdentifier();
				}
				string parameter = match.Groups[1].Value;
				return $"randomGetLong{randomGetLongSuffix}({parameter})";
			});

			// Replace Random.getFloat with randomGetFloat and track if found.
			source = Regex.Replace(source, getFloatPattern, match =>
			{
				foundGetFloat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetFloat("))
				{
					randomGetFloatSuffix = GetRandomMethodIdentifier();
				}
				string parameter = match.Groups[1].Value;
				return $"randomGetFloat{randomGetFloatSuffix}({parameter})";
			});

			// Replace Random.getDouble with randomGetDouble and track if found.
			source = Regex.Replace(source, getDoublePattern, match =>
			{
				foundGetDouble = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomGetDouble("))
				{
					randomGetDoubleSuffix = GetRandomMethodIdentifier();
				}
				string parameter = match.Groups[1].Value;
				return $"randomGetDouble{randomGetDoubleSuffix}({parameter})";
			});

			// Replace Random.fillBytes with randomFillBytes and track if found.
			source = Regex.Replace(source, fillBytesPattern, match =>
			{
				foundFillBytes = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("randomFillBytes("))
				{
					randomFillBytesSuffix = GetRandomMethodIdentifier();
				}
				string parameter = match.Groups[1].Value;
				return $"randomFillBytes{randomFillBytesSuffix}({parameter})";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <random> import.
			if (foundGetIntMinMax || foundGetLongMinMax || foundGetFloatMinMax || foundGetDoubleMinMax || foundGetInt || foundGetLong || foundGetFloat || foundGetDouble || foundFillBytes)
			{
				source = AddInclude(source, "random");
				if (foundGetFloat || foundGetDouble)
				{
					source = AddInclude(source, "limits");
				}
				if (foundFillBytes)
				{
					source = AddInclude(source, "vector");
				}

				// TODO: Store random locally.
			}

			// Append randomGetIntMinMax method if it was found.
			if (foundGetIntMinMax)
			{
				methods.AppendLine($"static int randomGetIntMinMax{randomGetIntMinMaxSuffix}(int min, int max)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_int_distribution<int> dist(min, max);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetLongMinMax method if it was found.
			if (foundGetLongMinMax)
			{
				methods.AppendLine($"static long randomGetLongMinMax{randomGetLongMinMaxSuffix}(long min, long max)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_int_distribution<long> dist(min, max);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetFloatMinMax method if it was found.
			if (foundGetFloatMinMax)
			{
				methods.AppendLine($"static float randomGetFloatMinMax{randomGetFloatMinMaxSuffix}(float min, float max)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_real_distribution<float> dist(min, max);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetDoubleMinMax method if it was found.
			if (foundGetDoubleMinMax)
			{
				methods.AppendLine($"static double randomGetDoubleMinMax{randomGetDoubleMinMaxSuffix}(double min, double max)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_real_distribution<double> dist(min, max);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetInt method if it was found.
			if (foundGetInt)
			{
				methods.AppendLine($"static int randomGetInt{randomGetIntSuffix}(int exclusiveMax)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_int_distribution<int> dist(0, exclusiveMax - 1);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetLong method if it was found.
			if (foundGetLong)
			{
				methods.AppendLine($"static long randomGetLong{randomGetLongSuffix}(long exclusiveMax)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_int_distribution<long> dist(0, exclusiveMax - 1);");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetFloat method if it was found.
			if (foundGetFloat)
			{
				methods.AppendLine($"static float randomGetFloat{randomGetFloatSuffix}(float exclusiveMax)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tsstd::uniform_real_distribution<float> dist(0.0f, exclusiveMax - std::numeric_limits<float>::epsilon());");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomGetDouble method if it was found.
			if (foundGetDouble)
			{
				methods.AppendLine($"static double randomGetDouble{randomGetDoubleSuffix}(double exclusiveMax)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_real_distribution<double> dist(0.0, exclusiveMax - std::numeric_limits<double>::epsilon());");
				methods.AppendLine("\treturn dist(gen);");
				methods.AppendLine("}\n");
			}

			// Append randomFillBytes method if it was found.
			if (foundFillBytes)
			{
				methods.AppendLine($"static void randomFillBytes{randomFillBytesSuffix}(std::vector<uint8_t>& bytes)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::random_device rd;");
				methods.AppendLine("\tstd::mt19937 gen(rd());");
				methods.AppendLine("\tstd::uniform_int_distribution<int> dist(0, 255); // Random byte range [0, 255].");
				methods.AppendLine("\tfor (auto& byte : bytes)");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tbyte = static_cast<uint8_t>(dist(gen)); // Assign random byte to each element.");
				methods.AppendLine("\t}");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
