// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using Breezy.Translators;
using System;
using System.IO;

namespace Breezy
{
	class Program
	{
		static void Main(string[] args)
		{
			if (args.Length == 0)
			{
				Console.WriteLine("Usage: Breezy <source1.bzy> <source2.bzy> ...");
				Console.WriteLine("Press any key to exit.");
				Console.ReadKey();
				return;
			}

			foreach (string filePath in args)
			{
				if (!File.Exists(filePath))
				{
					Console.WriteLine($"File not found: {filePath}");
					continue;
				}

				try
				{
					// Read the source file.
					string source = File.ReadAllText(filePath);

					// Do the translations.
					source = CollectionTranslator.Process(source);
					source = StringTranslator.Process(source);
					source = MainTranslator.Process(source);
					source = ConsoleTranslator.Process(source);
					source = FileTranslator.Process(source);
					source = EnhancedForTranslator.Process(source);

					// Write the translated code to a .cpp file.
					string outputFile = Path.ChangeExtension(filePath, ".cpp");
					File.WriteAllText(outputFile, source);

					Console.WriteLine($"C++ code generated: {outputFile}");
				}
				catch (Exception ex)
				{
					Console.WriteLine($"Error processing file {filePath}: {ex.Message}");
				}
			}

			Console.WriteLine("Processing complete.");
		}
	}
}
